#include <eigen3/Eigen/Core>

#include <numpy_eigen/boost_python_headers.hpp>
Eigen::Matrix<double, 6, 1> test_double_6_1(const Eigen::Matrix<double, 6, 1> & M)
{
	return M;
}
void export_double_6_1()
{
	boost::python::def("test_double_6_1",test_double_6_1);
}

