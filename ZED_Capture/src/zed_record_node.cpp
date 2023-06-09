#include "videocapture.hpp"
#include "sensorcapture.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>

#include <opencv2/opencv.hpp>

#include <ros/ros.h>
#include <rosbag/bag.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <rosbag/view.h>
#include <sensor_msgs/Image.h>
#include <std_msgs/Time.h>
#include <std_msgs/Header.h>
#include <sensor_msgs/Imu.h>
#include <tf2/LinearMath/Quaternion.h>


// ----> Functions
// Sensor acquisition runs at 400Hz, so it must be executed in a different thread
void getSensorThreadFunc(sl_oc::sensors::SensorCapture* sensCap);
// <---- Functions

// ----> Global variables
std::mutex imuMutex;
std::mutex bagMutex;
std::string imuTsStr;
std::string imuAccelStr;
std::string imuGyroStr;
sensor_msgs::Imu imu_msg;
cv_bridge::CvImage cvImage;

#define k_JPEG_COMPRESS     97

// rosbag recoder
rosbag::Bag bag_out;

bool sensThreadStop=false;
uint64_t mcu_sync_ts=0;


int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    ros::init(argc, argv, "zed_bag_record");

    ros::start();

    //sl_oc::sensors::SensorCapture::resetSensorModule();
    //sl_oc::sensors::SensorCapture::resetVideoModule();

    // Set the verbose level
    sl_oc::VERBOSITY verbose = sl_oc::VERBOSITY::ERROR;

    // ----> Set the video parameters
    sl_oc::video::VideoParams params;
    params.res = sl_oc::video::RESOLUTION::HD1080;
    params.fps = sl_oc::video::FPS::FPS_30;
    params.verbose = verbose;
    // <---- Video parameters

    bag_out.open("zed_raw.bag", rosbag::bagmode::Write);

    // ----> Create a Video Capture object
    sl_oc::video::VideoCapture videoCap(params);
    if( !videoCap.initializeVideo(-1) )
    {
        std::cerr << "Cannot open camera video capture" << std::endl;
        std::cerr << "Try to enable verbose to get more info" << std::endl;

        return EXIT_FAILURE;
    }

    // Serial number of the connected camera
    int camSn = videoCap.getSerialNumber();

    std::cout << "Video Capture connected to camera sn: " << camSn << std::endl;
    // <---- Create a Video Capture object

    // ----> Create a Sensors Capture object
    sl_oc::sensors::SensorCapture sensCap(verbose);
    if( !sensCap.initializeSensors(camSn) ) // Note: we use the serial number acquired by the VideoCapture object
    {
        std::cerr << "Cannot open sensors capture" << std::endl;
        std::cerr << "Try to enable verbose to get more info" << std::endl;

        return EXIT_FAILURE;
    }
    std::cout << "Sensors Capture connected to camera sn: " << sensCap.getSerialNumber() << std::endl;

    // Start the sensor capture thread. Note: since sensor data can be retrieved at 400Hz and video data frequency is
    // minor (max 100Hz), we use a separated thread for sensors.
    std::thread sensThread(getSensorThreadFunc,&sensCap);
    // <---- Create Sensors Capture

    // ----> Enable video/sensors synchronization
    videoCap.enableSensorSync(&sensCap);
    // <---- Enable video/sensors synchronization

    // ----> Init OpenCV RGB frame
    int w,h;
    videoCap.getFrameSize(w,h);

    cv::Size display_resolution(1024, 576);

    switch(params.res)
    {
    default:
    case sl_oc::video::RESOLUTION::VGA:
        display_resolution.width = w;
        display_resolution.height = h;
        break;
    case sl_oc::video::RESOLUTION::HD720:
        display_resolution.width = w*0.6;
        display_resolution.height = h*0.6;
        break;
    case sl_oc::video::RESOLUTION::HD1080:
    case sl_oc::video::RESOLUTION::HD2K:
        display_resolution.width = w*0.4;
        display_resolution.height = h*0.4;
        break;
    }

    int h_data = 70;
    cv::Mat frameDisplay(display_resolution.height + h_data, display_resolution.width,CV_8UC3, cv::Scalar(0,0,0));
    cv::Mat frameData = frameDisplay(cv::Rect(0,0, display_resolution.width, h_data));
    cv::Mat frameBGRDisplay = frameDisplay(cv::Rect(0,h_data, display_resolution.width, display_resolution.height));
    cv::Mat frameBGR(h, w, CV_8UC3, cv::Scalar(0,0,0));
    // <---- Init OpenCV RGB frame

    uint64_t last_timestamp = 0;

    float frame_fps=0;

    std::vector<int> encode_param;
    encode_param.push_back(cv::IMWRITE_JPEG_QUALITY) ;
    encode_param.push_back(k_JPEG_COMPRESS);

    // Infinite grabbing loop
    while (ros::ok())
    {
        // ----> Get Video frame
        // Get last available frame
        const sl_oc::video::Frame frame = videoCap.getLastFrame(1);

        // If the frame is valid we can update it
        std::stringstream videoTs;
        if(frame.data!=nullptr && frame.timestamp!=last_timestamp)
        {
            frame_fps = 1e9/static_cast<float>(frame.timestamp-last_timestamp);
            last_timestamp = frame.timestamp;

            // ----> Conversion from YUV 4:2:2 to BGR for visualization
            cv::Mat frameYUV( frame.height, frame.width, CV_8UC2, frame.data);
            cv::cvtColor(frameYUV,frameBGR, cv::COLOR_YUV2BGR_YUYV);
            // <---- Conversion from YUV 4:2:2 to BGR for visualization
        }
        // <---- Get Video frame

        // ----> Video Debug information
        videoTs << std::fixed << std::setprecision(9) << "Video timestamp: " << static_cast<double>(last_timestamp)/1e9<< " sec" ;
        if( last_timestamp!=0 )
            videoTs << std::fixed << std::setprecision(1)  << " [" << frame_fps << " Hz]";
        // <---- Video Debug information

        // ----> Display frame with info
        if(frame.data!=nullptr)
        {
            frameData.setTo(0);

            // Video info
            cv::putText( frameData, videoTs.str(), cv::Point(10,20),cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(241,240,236));

            // IMU info
            imuMutex.lock();
//            bagMutex.lock();

            uint64_t nsecs = (frame.timestamp % 1000) * 1000 * 1000;
            uint64_t secs = frame.timestamp / 1000;

            // Bag file write
            std::vector<uchar> buff;
            cv::Mat m = frameBGR;
            cv::imencode(".jpg",m,buff, encode_param);
            cvImage.image = m;
            cvImage.encoding = sensor_msgs::image_encodings::RGB8;
            cvImage.header.stamp = ros::Time::now();
            // if (cvImage.header.stamp.toNSec() == 0) cvImage.header.stamp = ros::TIME_MIN;
            bag_out.write("/camera/image_raw", cvImage.header.stamp, cvImage.toImageMsg());

            cv::putText( frameData, imuTsStr, cv::Point(10, 35),cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(241,240,236));

            // Timestamp offset info
            std::stringstream offsetStr;
            double offset = (static_cast<double>(frame.timestamp)-static_cast<double>(mcu_sync_ts))/1e9;
            offsetStr << std::fixed << std::setprecision(9) << std::showpos << "Timestamp offset: " << offset << " sec [video-sensors]";
            cv::putText( frameData, offsetStr.str().c_str(), cv::Point(10, 50),cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(241,240,236));

            // Average timestamp offset info (we wait at least 200 frames to be sure that offset is stable)
            if( frame.frame_id>200 )
            {
                static double sum=0;
                static int count=0;

                sum += offset;
                double avg_offset=sum/(++count);

                std::stringstream avgOffsetStr;
                avgOffsetStr << std::fixed << std::setprecision(9) << std::showpos << "Avg timestamp offset: " << avg_offset << " sec";
                cv::putText( frameData, avgOffsetStr.str().c_str(), cv::Point(10,62),cv::FONT_HERSHEY_SIMPLEX,0.35, cv::Scalar(241, 240,236));
            }

            // IMU values
            cv::putText( frameData, "Inertial sensor data:", cv::Point(display_resolution.width/2,20),cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(241, 240,236));
            cv::putText( frameData, imuAccelStr, cv::Point(display_resolution.width/2+15,42),cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(241, 240,236));
            cv::putText( frameData, imuGyroStr, cv::Point(display_resolution.width/2+15, 62),cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(241, 240,236));

            imuMutex.unlock();
//            bagMutex.unlock();

            // Resize Image for display
            cv::resize(frameBGR, frameBGRDisplay, display_resolution);
            // Display image
            cv::imshow( "Stream RGB", frameDisplay);
        }
        // <---- Display frame with info

        // ----> Keyboard handling
        int key = cv::waitKey(1);

        if( key != -1 )
        {
            // Quit
            if(key=='q' || key=='Q'|| key==27)
            {
                sensThreadStop=true;
                sensThread.join();
                break;
            }
            bag_out.close();
        }
        // <---- Keyboard handling
    }

    // ros::shutdown();s

    return EXIT_SUCCESS;
}

// Sensor acquisition runs at 400Hz, so it must be executed in a different thread
void getSensorThreadFunc(sl_oc::sensors::SensorCapture* sensCap)
{
    // Flag to stop the thread
    sensThreadStop = false;

    // Previous IMU timestamp to calculate frequency
    uint64_t last_imu_ts = 0;

    // Infinite data grabbing loop
    while(!sensThreadStop)
    {
        // ----> Get IMU data
        const sl_oc::sensors::data::Imu imuData = sensCap->getLastIMUData(2000);

        // Process data only if valid
        if(imuData.valid == sl_oc::sensors::data::Imu::NEW_VAL ) // Uncomment to use only data syncronized with the video frames
        {
            // ----> Data info to be displayed
            std::stringstream timestamp;
            std::stringstream accel;
            std::stringstream gyro;

            timestamp << std::fixed << std::setprecision(9) << "IMU timestamp:   " << static_cast<double>(imuData.timestamp)/1e9<< " sec" ;
            if(last_imu_ts!=0)
                timestamp << std::fixed << std::setprecision(1)  << " [" << 1e9/static_cast<float>(imuData.timestamp-last_imu_ts) << " Hz]";
            last_imu_ts = imuData.timestamp;

            accel << std::fixed << std::showpos << std::setprecision(4) << " * Accel: " << imuData.aX << " " << imuData.aY << " " << imuData.aZ << " [m/s^2]";
            gyro << std::fixed << std::showpos << std::setprecision(4) << " * Gyro: " << imuData.gX << " " << imuData.gY << " " << imuData.gZ << " [deg/s]";
            // <---- Data info to be displayed

//            bagMutex.lock();

            uint64_t nsecs = (last_imu_ts % 1000) * 1000 * 1000;
            uint64_t secs = last_imu_ts / 1000;

            imu_msg.header.frame_id = "/stereo_imu";

            imu_msg.header.stamp = ros::Time::now();
            // if (imu_msg.header.stamp.toNSec() == 0) imu_msg.header.stamp = ros::TIME_MIN;

            imu_msg.linear_acceleration.x = imuData.aX;
            imu_msg.linear_acceleration.y = imuData.aY;
            imu_msg.linear_acceleration.z = imuData.aZ;
            imu_msg.angular_velocity.x = imuData.gX;
            imu_msg.angular_velocity.y = imuData.gY;
            imu_msg.angular_velocity.z = imuData.gZ;

            bag_out.write("/imu/imu_raw", imu_msg.header.stamp, imu_msg);

//            bagMutex.unlock();

            // Mutex to not overwrite data while diplaying them
            imuMutex.lock();

            imuTsStr = timestamp.str();
            imuAccelStr = accel.str();
            imuGyroStr = gyro.str();

            // ----> Timestamp of the synchronized data
            if(imuData.sync)
            {
                mcu_sync_ts = imuData.timestamp;
            }
            // <---- Timestamp of the synchronized data

            imuMutex.unlock();
        }
        // <---- Get IMU data
    }
}
