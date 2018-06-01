#include "SpaceTimeMatrix.hpp"


// TODO:
//	- Add option to set h^l ~ dt^k, where l is the spatial order and k
//	  time order, that way accuracy same in space and time



class MySpaceTime : public SpaceTimeMatrix
{
private:

	int m_refLevels;
	int m_order;
	int m_dim;
	int* m_M_rowptr;
	int* m_M_colinds;
	double* m_M_data;

	void getSpatialDiscretization(const MPI_Comm &spatialComm, int *&A_rowptr,
                                  int *&A_colinds, double *&A_data, double *&B,
                                  double *&X, int &localMinRow, int &localMaxRow,
                                  int &spatialDOFs, double t);
	void getSpatialDiscretization(int *&A_rowptr, int *&A_colinds, double *&A_data,
                                  double *&B, double *&X, int &spatialDOFs, double t);
	void getMassMatrix(int* &M_rowptr, int* &M_colinds, double* &M_data);
    // virtual void getRHS(const MPI_Comm &spatialComm, double *&B, double t);

public:

	MySpaceTime(MPI_Comm globComm, int timeDisc, int numTimeSteps);
	MySpaceTime(MPI_Comm globComm, int timeDisc, int numTimeSteps,
				double t0, double t1);
	MySpaceTime(MPI_Comm globComm, int timeDisc, int numTimeSteps,
				int refLevels, int order);
	MySpaceTime(MPI_Comm globComm, int timeDisc, int numTimeSteps,
				double t0, double t1, int refLevels, int order);
    ~MySpaceTime() { };

};