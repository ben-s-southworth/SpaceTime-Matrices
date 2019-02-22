#include <iostream>
#include <fstream>
#include <map>
#include <algorithm>
#include "SpaceTimeMatrix.hpp"

// TODO:
//      - Add isTimeDependent option
//      - Add hypre timing for setup and solve (see ij.c)



SpaceTimeMatrix::SpaceTimeMatrix(MPI_Comm globComm, int timeDisc,
                                 int numTimeSteps, double dt)
    : m_globComm{globComm}, m_timeDisc{timeDisc}, m_numTimeSteps{numTimeSteps},
      m_dt{dt}, m_solver(NULL), m_gmres(NULL), m_bij(NULL), m_xij(NULL), 
      m_M_rowptr(NULL), m_M_colinds(NULL), m_M_data(NULL), m_rebuildSolver(false),
      m_bsize(1), m_hmin(-1), m_hmax(-1)
{
    // Get number of processes
    MPI_Comm_rank(m_globComm, &m_globRank);
    MPI_Comm_size(m_globComm, &m_numProc);

    // Set member variables
    if (m_globRank == 0) {
        std::cout << "dt = " << m_dt << "\n";
    }

    // Check that number of time steps divides the number MPI processes or vice versa.
    if (m_numTimeSteps <= m_numProc) {
        m_useSpatialParallel = true;
        if (m_numProc % m_numTimeSteps != 0) {
            if (m_globRank == 0) {
                std::cout << "Error: number of time steps does not divide number of processes.\n";
            }
            MPI_Finalize();
            return;
        }
        else {
            m_Np_x = m_numProc / m_numTimeSteps;
        }

        // Set up communication group for spatial discretizations.
        m_timeInd = m_globRank / m_Np_x;
        MPI_Comm_split(m_globComm, m_timeInd, m_globRank, &m_spatialComm);
        MPI_Comm_rank(m_spatialComm, &m_spatialRank);
        MPI_Comm_size(m_spatialComm, &m_spCommSize);

    }
    else {
        m_useSpatialParallel = false;
        if (m_numTimeSteps % m_numProc  != 0) {
            if (m_globRank == 0) {
                std::cout << "Error: number of processes does not divide number of time steps.\n";
            }
            MPI_Finalize();
            return;
        }
        // Time steps computed per processor. 
        m_ntPerProc = m_numTimeSteps / m_numProc;
    }
}


SpaceTimeMatrix::~SpaceTimeMatrix()
{
    if (m_solver) HYPRE_BoomerAMGDestroy(m_solver);
    if (m_gmres) HYPRE_ParCSRGMRESDestroy(m_gmres);
    if (m_Aij) HYPRE_IJMatrixDestroy(m_Aij);   // This destroys parCSR matrix too
    if (m_bij) HYPRE_IJVectorDestroy(m_bij);   // This destroys parVector too
    if (m_xij) HYPRE_IJVectorDestroy(m_xij);
}


void SpaceTimeMatrix::BuildMatrix()
{
    if (m_globRank == 0) std::cout << "Building matrix, " << m_useSpatialParallel << "\n";
    if (m_useSpatialParallel) GetMatrix_ntLE1();
    else GetMatrix_ntGT1();
    if (m_globRank == 0) std::cout << "Space-time matrix assembled.\n";
}


/* Get space-time matrix for at most 1 time step per processor */
void SpaceTimeMatrix::GetMatrix_ntLE1()
{
    // Get local CSR structure
    int* rowptr;
    int* colinds;
    double* data;
    double* B;
    double* X;
    int localMinRow;
    int localMaxRow;
    int spatialDOFs;
    if (m_timeDisc == 11) {
        BDF1(rowptr, colinds, data, B, X, localMinRow, localMaxRow, spatialDOFs);
    }
    else if (m_timeDisc == 31) {
        AB1(rowptr, colinds, data, B, X, localMinRow, localMaxRow, spatialDOFs);
    }
    else {
        std::cout << "WARNING: invalid choice of time integration.\n";
        MPI_Finalize();
        return;
    }

    // Initialize matrix
    int onProcSize = localMaxRow - localMinRow + 1;
    int ilower = m_timeInd*spatialDOFs + localMinRow;
    int iupper = m_timeInd*spatialDOFs + localMaxRow;
    HYPRE_IJMatrixCreate(m_globComm, ilower, iupper, ilower, iupper, &m_Aij);
    HYPRE_IJMatrixSetObjectType(m_Aij, HYPRE_PARCSR);
    HYPRE_IJMatrixInitialize(m_Aij);

    // Set matrix coefficients
    int* rows = new int[onProcSize];
    int* cols_per_row = new int[onProcSize];
    for (int i=0; i<onProcSize; i++) {
        rows[i] = ilower + i;
        cols_per_row[i] = rowptr[i+1] - rowptr[i];
    }
    HYPRE_IJMatrixSetValues(m_Aij, onProcSize, cols_per_row, rows, colinds, data);

    // Finalize construction
    HYPRE_IJMatrixAssemble(m_Aij);
    HYPRE_IJMatrixGetObject(m_Aij, (void **) &m_A);

    /* Create rhs and solution vectors */
    HYPRE_IJVectorCreate(m_globComm, ilower, iupper, &m_bij);
    HYPRE_IJVectorSetObjectType(m_bij, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(m_bij);
    HYPRE_IJVectorSetValues(m_bij, onProcSize, rows, B);
    HYPRE_IJVectorAssemble(m_bij);
    HYPRE_IJVectorGetObject(m_bij, (void **) &m_b);

    HYPRE_IJVectorCreate(m_globComm, ilower, iupper, &m_xij);
    HYPRE_IJVectorSetObjectType(m_xij, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(m_xij);
    HYPRE_IJVectorSetValues(m_xij, onProcSize, rows, X);
    HYPRE_IJVectorAssemble(m_xij);
    HYPRE_IJVectorGetObject(m_xij, (void **) &m_x);

    // Remove pointers that should have been copied by Hypre
    delete[] rowptr;
    delete[] colinds;
    delete[] data;
    delete[] B;
    delete[] X;
    delete[] rows;
    delete[] cols_per_row;
}


/* Get space-time matrix for more than 1 time step per processor */
void SpaceTimeMatrix::GetMatrix_ntGT1()
{
    // Get local CSR structure
    int* rowptr;
    int* colinds;
    double* data;
    double* B;
    double* X;
    int onProcSize;
    if (m_timeDisc == 11) {
        BDF1(rowptr, colinds, data, B, X, onProcSize);
    }
    else if (m_timeDisc == 31) {
        AB1(rowptr, colinds, data, B, X, onProcSize);
    }
    else {
        std::cout << "WARNING: invalid choice of time integration.\n";
        MPI_Finalize();
        return;
    }

    // Initialize matrix
    int ilower = m_globRank*onProcSize;
    int iupper = (m_globRank+1)*onProcSize - 1;
    HYPRE_IJMatrixCreate(MPI_COMM_WORLD, ilower, iupper, ilower, iupper, &m_Aij);
    HYPRE_IJMatrixSetObjectType(m_Aij, HYPRE_PARCSR);
    HYPRE_IJMatrixInitialize(m_Aij);

    // Set matrix coefficients
    int* rows = new int[onProcSize];
    int* cols_per_row = new int[onProcSize];
    for (int i=0; i<onProcSize; i++) {
        rows[i] = ilower + i;
        cols_per_row[i] = rowptr[i+1] - rowptr[i];
    }
    HYPRE_IJMatrixSetValues(m_Aij, onProcSize, cols_per_row, rows, colinds, data);

    // Finalize construction
    HYPRE_IJMatrixAssemble(m_Aij);
    HYPRE_IJMatrixGetObject(m_Aij, (void **) &m_A);

    /* Create sample rhs and solution vectors */
    HYPRE_IJVectorCreate(m_globComm, ilower, iupper, &m_bij);
    HYPRE_IJVectorSetObjectType(m_bij, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(m_bij);
    HYPRE_IJVectorSetValues(m_bij, onProcSize, rows, B);
    HYPRE_IJVectorAssemble(m_bij);
    HYPRE_IJVectorGetObject(m_bij, (void **) &m_b);

    HYPRE_IJVectorCreate(m_globComm, ilower, iupper, &m_xij);
    HYPRE_IJVectorSetObjectType(m_xij, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(m_xij);
    HYPRE_IJVectorSetValues(m_xij, onProcSize, rows, X);
    HYPRE_IJVectorAssemble(m_xij);
    HYPRE_IJVectorGetObject(m_xij, (void **) &m_x);

    // Remove pointers that should have been copied by Hypre
    delete[] rowptr;
    delete[] colinds;
    delete[] data;
    delete[] B;
    delete[] X;
    delete[] rows;
    delete[] cols_per_row;
}


/* Set classical AMG parameters for BoomerAMG solve. */
void SpaceTimeMatrix::SetAMG()
{
   m_solverOptions.prerelax = "AA";
   m_solverOptions.postrelax = "AA";
   m_solverOptions.relax_type = 3;
   m_solverOptions.interp_type = 6;
   m_solverOptions.strength_tolC = 0.1;
   m_solverOptions.coarsen_type = 6;
   m_solverOptions.distance_R = -1;
   m_solverOptions.strength_tolR = -1;
   m_solverOptions.filter_tolA = 0.0;
   m_solverOptions.filter_tolR = 0.0;
   m_solverOptions.cycle_type = 1;
   m_rebuildSolver = true;
}


/* Set standard AIR parameters for BoomerAMG solve. */
void SpaceTimeMatrix::SetAIR()
{
   m_solverOptions.prerelax = "A";
   m_solverOptions.postrelax = "FFC";
   m_solverOptions.relax_type = 3;
   m_solverOptions.interp_type = 100;
   m_solverOptions.strength_tolC = 0.005;
   m_solverOptions.coarsen_type = 6;
   m_solverOptions.distance_R = 1.5;
   m_solverOptions.strength_tolR = 0.005;
   m_solverOptions.filter_tolA = 0.0;
   m_solverOptions.filter_tolR = 0.0;
   m_solverOptions.cycle_type = 1;
   m_rebuildSolver = true;
}


/* Set AIR parameters assuming triangular matrix in BoomerAMG solve. */
void SpaceTimeMatrix::SetAIRHyperbolic()
{
   m_solverOptions.prerelax = "A";
   m_solverOptions.postrelax = "F";
   m_solverOptions.relax_type = 10;
   m_solverOptions.interp_type = 100;
   m_solverOptions.strength_tolC = 0.005;
   m_solverOptions.coarsen_type = 6;
   m_solverOptions.distance_R = 1.5;
   m_solverOptions.strength_tolR = 0.005;
   m_solverOptions.filter_tolA = 0.0001;
   m_solverOptions.filter_tolR = 0.0;
   m_solverOptions.cycle_type = 1;
   m_rebuildSolver = true;
}


/* Provide BoomerAMG parameters struct for solve. */
void SpaceTimeMatrix::SetAMGParameters(AMG_parameters &params)
{
    // TODO: does this copy the structure by value?
    m_solverOptions = params;
}


void SpaceTimeMatrix::PrintMeshData()
{
    if (m_globRank == 0) {
        std::cout << "Space-time mesh:\n\thmin = " << m_hmin <<
        "\n\thmax = " << m_hmax << "\n\tdt   = " << m_dt << "\n\n";
    }
}


/* Initialize AMG solver based on parameters in m_solverOptions struct. */
void SpaceTimeMatrix::SetupBoomerAMG(int printLevel, int maxiter, double tol)
{
    // If solver exists and rebuild bool is false, return
    if (m_solver && !m_rebuildSolver){
        return;
    }
    // Build/rebuild solver
    else {
        if (m_solver) {
            std::cout << "Rebuilding solver.\n";
            HYPRE_BoomerAMGDestroy(m_solver);
        }

        // Array to store relaxation scheme and pass to Hypre
        //      TODO: does hypre clean up grid_relax_points
        int ns_down = m_solverOptions.prerelax.length();
        int ns_up = m_solverOptions.postrelax.length();
        int ns_coarse = 1;
        std::string Fr("F");
        std::string Cr("C");
        std::string Ar("A");
        int* *grid_relax_points = new int* [4];
        grid_relax_points[0] = NULL;
        grid_relax_points[1] = new int[ns_down];
        grid_relax_points[2] = new int [ns_up];
        grid_relax_points[3] = new int[1];
        grid_relax_points[3][0] = 0;

        // set down relax scheme 
        for(unsigned int i = 0; i<ns_down; i++) {
            if (m_solverOptions.prerelax.compare(i,1,Fr) == 0) {
                grid_relax_points[1][i] = -1;
            }
            else if (m_solverOptions.prerelax.compare(i,1,Cr) == 0) {
                grid_relax_points[1][i] = 1;
            }
            else if (m_solverOptions.prerelax.compare(i,1,Ar) == 0) {
                grid_relax_points[1][i] = 0;
            }
        }

        // set up relax scheme 
        for(unsigned int i = 0; i<ns_up; i++) {
            if (m_solverOptions.postrelax.compare(i,1,Fr) == 0) {
                grid_relax_points[2][i] = -1;
            }
            else if (m_solverOptions.postrelax.compare(i,1,Cr) == 0) {
                grid_relax_points[2][i] = 1;
            }
            else if (m_solverOptions.postrelax.compare(i,1,Ar) == 0) {
                grid_relax_points[2][i] = 0;
            }
        }

        // Create preconditioner
        HYPRE_BoomerAMGCreate(&m_solver);
        HYPRE_BoomerAMGSetTol(m_solver, tol);    
        HYPRE_BoomerAMGSetMaxIter(m_solver, maxiter);
        HYPRE_BoomerAMGSetPrintLevel(m_solver, printLevel);

        if (m_solverOptions.distance_R > 0) {
            HYPRE_BoomerAMGSetRestriction(m_solver, m_solverOptions.distance_R);
            HYPRE_BoomerAMGSetStrongThresholdR(m_solver, m_solverOptions.strength_tolR);
            HYPRE_BoomerAMGSetFilterThresholdR(m_solver, m_solverOptions.filter_tolR);
        }
        HYPRE_BoomerAMGSetInterpType(m_solver, m_solverOptions.interp_type);
        HYPRE_BoomerAMGSetCoarsenType(m_solver, m_solverOptions.coarsen_type);
        HYPRE_BoomerAMGSetAggNumLevels(m_solver, 0);
        HYPRE_BoomerAMGSetStrongThreshold(m_solver, m_solverOptions.strength_tolC);
        HYPRE_BoomerAMGSetGridRelaxPoints(m_solver, grid_relax_points);
        if (m_solverOptions.relax_type > -1) {
            HYPRE_BoomerAMGSetRelaxType(m_solver, m_solverOptions.relax_type);
        }
        HYPRE_BoomerAMGSetCycleNumSweeps(m_solver, ns_coarse, 3);
        HYPRE_BoomerAMGSetCycleNumSweeps(m_solver, ns_down,   1);
        HYPRE_BoomerAMGSetCycleNumSweeps(m_solver, ns_up,     2);
        if (m_solverOptions.filter_tolA > 0) {
            HYPRE_BoomerAMGSetADropTol(m_solver, m_solverOptions.filter_tolA);
        }
        // type = -1: drop based on row inf-norm
        else if (m_solverOptions.filter_tolA == -1) {
            HYPRE_BoomerAMGSetADropType(m_solver, -1);
        }

        // Do not rebuild solver unless parameters are changed.
        m_rebuildSolver = false;

        // Set cycle type for solve 
        HYPRE_BoomerAMGSetCycleType(m_solver, m_solverOptions.cycle_type);
    }
}


void SpaceTimeMatrix::SolveAMG(double tol, int maxiter, int printLevel,
                               bool binv_scale)
{
    SetupBoomerAMG(printLevel, maxiter, tol);

    if (binv_scale) {
        HYPRE_ParCSRMatrix A_s;
        hypre_ParcsrBdiagInvScal(m_A, m_bsize, &A_s);
        hypre_ParCSRMatrixDropSmallEntries(A_s, 1e-15, 1);
        HYPRE_ParVector b_s;
        hypre_ParvecBdiagInvScal(m_b, m_bsize, &b_s, m_A);

        HYPRE_BoomerAMGSetup(m_solver, A_s, b_s, m_x);
        HYPRE_BoomerAMGSolve(m_solver, A_s, b_s, m_x);
    }
    else {
        HYPRE_BoomerAMGSetup(m_solver, m_A, m_b, m_x);
        HYPRE_BoomerAMGSolve(m_solver, m_A, m_b, m_x);
    }
}


void SpaceTimeMatrix::SolveGMRES(double tol, int maxiter, int printLevel,
                                 bool binv_scale, int precondition, int AMGiters) 
{
    HYPRE_ParCSRGMRESCreate(m_globComm, &m_gmres);
    
    // AMG preconditioning (setup boomerAMG with 1 max iter and print level 1)
    if (precondition == 1) {
        SetupBoomerAMG(1, AMGiters, 0.0);
        HYPRE_GMRESSetPrecond(m_gmres, (HYPRE_PtrToSolverFcn) HYPRE_BoomerAMGSolve,
                          (HYPRE_PtrToSolverFcn) HYPRE_BoomerAMGSetup, m_solver);
    }
    // Diagonally scaled preconditioning?
    else if (precondition == 2) {
        HYPRE_GMRESSetPrecond(m_gmres, (HYPRE_PtrToSolverFcn) HYPRE_ParCSROnProcTriSolve,
                             (HYPRE_PtrToSolverFcn) HYPRE_ParCSROnProcTriSetup, m_solver);  
    }

    HYPRE_GMRESSetKDim(m_gmres, 5);
    HYPRE_GMRESSetMaxIter(m_gmres, maxiter);
    HYPRE_GMRESSetTol(m_gmres, tol);
    HYPRE_GMRESSetPrintLevel(m_gmres, printLevel);
    HYPRE_GMRESSetLogging(m_gmres, 1);

   if (binv_scale) {
        HYPRE_ParCSRMatrix A_s;
        hypre_ParcsrBdiagInvScal(m_A, m_bsize, &A_s);
        hypre_ParCSRMatrixDropSmallEntries(A_s, 1e-15, 1);
        HYPRE_ParVector b_s;
        hypre_ParvecBdiagInvScal(m_b, m_bsize, &b_s, m_A);
        HYPRE_ParCSRGMRESSetup(m_gmres, A_s, b_s, m_x);
        HYPRE_ParCSRGMRESSolve(m_gmres, A_s, b_s, m_x);
    }
    else {
        HYPRE_ParCSRGMRESSetup(m_gmres, m_A, m_b, m_x);
        HYPRE_ParCSRGMRESSolve(m_gmres, m_A, m_b, m_x);
    }
}


// TODO : this function may be unnecessary if mass matrix is stored as member variables...
void SpaceTimeMatrix::getMassMatrix(int* &M_rowptr, int* &M_colinds, double* &M_data)
{
    // TODO : set to sparse identity matrix here
    if ((!m_M_rowptr) || (!m_M_colinds) || (!m_M_data)) {

    }

    // TODO : set input pointers to address of member variables

}

/* ------------------------------------------------------------------------- */
/* ----------------- More than one time step per processor ----------------- */
/* ------------------------------------------------------------------------- */

/* First-order BDF implicit scheme (Backward Euler / 1st-order Adams-Moulton). */
void SpaceTimeMatrix::BDF1(int* &rowptr, int* &colinds, double* &data,
                           double* &B, double* &X, int &onProcSize)
{
    int tInd0 = m_globRank*m_ntPerProc;
    int tInd1 = tInd0 + m_ntPerProc -1;

    // Get spatial discretization for first time step on this processor
    int* T_rowptr;
    int* T_colinds;
    double* T_data;
    double* B0;
    double* X0;
    int spatialDOFs;
    getSpatialDiscretization(T_rowptr, T_colinds, T_data, B0,
                             X0, spatialDOFs, m_dt*tInd0, m_bsize);
    int nnzPerTime = T_rowptr[spatialDOFs];    

    // Get mass matrix and scale by 1/dt
    int* M_rowptr;
    int* M_colinds;
    double* M_data;
    getMassMatrix(M_rowptr, M_colinds, M_data);
    for (int i=0; i<M_rowptr[spatialDOFs]; i++) {
        M_data[i] /= m_dt;
    }

    // Get size/nnz of spatial discretization and total for rows on this processor.
    // Allocate CSR structure.
    onProcSize  = m_ntPerProc * spatialDOFs;
    int procNnz = m_ntPerProc * (M_rowptr[spatialDOFs] + nnzPerTime);     // nnzs on this processor
    // Account for only identity at time t=0
    if (tInd0 == 0) procNnz -= (M_rowptr[spatialDOFs] + nnzPerTime - spatialDOFs);

    rowptr  = new int[onProcSize + 1];
    colinds = new int[procNnz];
    data = new double[procNnz];
    B = new double[onProcSize];
    X = new double[onProcSize];
    int dataInd = 0;
    int thisRow = 0;
    rowptr[0] = 0;

    // Loop over each time index and build sparse space-time matrix rows on this processor
    for (int ti=tInd0; ti<=tInd1; ti++) {

        // Build spatial matrix for next time step if coefficients are time dependent
        // (m_isTimeDependent) and matrix has not been built yet (ti > tInd0)
        if ((ti > tInd0) && m_isTimeDependent) {
            delete[] T_rowptr;
            delete[] T_colinds;
            delete[] T_data;
            delete[] B0;
            delete[] X0;
            getSpatialDiscretization(T_rowptr, T_colinds, T_data, B0,
                                     X0, spatialDOFs, m_dt*ti, m_bsize);
        }

        int colPlusOffd  = (ti - 1)*spatialDOFs;
        int colPlusDiag  = ti*spatialDOFs;
        std::map<int, double>::iterator it;

        // At time t=0, have fixed initial condition. Set matrix to identity
        // and fix RHS and initial guess to ICs.
        if (ti == 0) {
            
            // Add initial condition to rhs
            addInitialCondition(B);

            // Loop over each row in spatial discretization at time ti
            for (int i=0; i<spatialDOFs; i++) {

                colinds[dataInd] = i;
                data[dataInd] = 1.0;
                dataInd += 1;
                rowptr[thisRow+1] = dataInd;
                thisRow += 1;

                // Set solution equal to initial condition
                X[thisRow] = B[i];
            }
        }
        else {

            // Loop over each row in spatial discretization at time ti
            for (int i=0; i<spatialDOFs; i++) {
                
                std::map<int, double> entries;

                // Add row of off-diagonal block, -(M/dt)*u_{i-1}
                for (int j=M_rowptr[i]; j<M_rowptr[i+1]; j++) {                
                    if (std::abs(M_data[j]) > 1e-16){
                        colinds[dataInd] = colPlusOffd + M_colinds[j];
                        data[dataInd] = -M_data[j];
                        dataInd += 1;
                        entries[M_colinds[j]] += M_data[j];
                    }
                }

                // Get row of spatial discretization
                for (int j=T_rowptr[i]; j<T_rowptr[i+1]; j++) {
                    entries[T_colinds[j]] += T_data[j];
                }

                // Add spatial discretization and mass matrix to global matrix
                for (it=entries.begin(); it!=entries.end(); it++) {
                    if (std::abs(it->second) > 1e-16) {
                        colinds[dataInd] = colPlusDiag + it->first;
                        data[dataInd] = it->second;
                        dataInd += 1;
                    }
                }

                // Add right hand side and initial guess for this row to global problem
                B[thisRow] = B0[i];
                X[thisRow] = X0[i];

                // Total nonzero for this row on processor is one for off-diagonal block
                // and the total nnz in this row of T (T_rowptr[i+1] - T_rowptr[i]).
                rowptr[thisRow+1] = dataInd;
                thisRow += 1;
            }
        }

        // Check if sufficient data was allocated
        if (dataInd > procNnz) {
            std::cout << "WARNING: Matrix has more nonzeros than allocated.\n";
        }
    }
    delete[] T_rowptr;
    delete[] T_colinds;
    delete[] T_data;
    delete[] M_rowptr;
    delete[] M_colinds;
    delete[] M_data;
    delete[] B0;
    delete[] X0;
}


/* First-order Adams-Bashforth explicit scheme (Forward Euler). */
void SpaceTimeMatrix::AB1(int* &rowptr, int* &colinds, double* &data,
                          double* &B, double* &X, int &onProcSize)
{
    int tInd0 = m_globRank*m_ntPerProc;
    int tInd1 = tInd0 + m_ntPerProc -1;

    // Get spatial discretization for first time step on this processor
    int* T_rowptr;
    int* T_colinds;
    double* T_data;
    double* B0;
    double* X0;
    int spatialDOFs;
    if (tInd0 > 0) {
        getSpatialDiscretization(T_rowptr, T_colinds, T_data, B0, X0,
                                 spatialDOFs, m_dt*(tInd0-1), m_bsize);
    }
    else {
        getSpatialDiscretization(T_rowptr, T_colinds, T_data, B0, X0,
                                 spatialDOFs, m_dt*tInd0, m_bsize);
    }
    int nnzPerTime = T_rowptr[spatialDOFs];    
 
    // Get mass matrix and scale by 1/dt
    int* M_rowptr;
    int* M_colinds;
    double* M_data;
    getMassMatrix(M_rowptr, M_colinds, M_data);
    for (int i=0; i<M_rowptr[spatialDOFs]; i++) {
        M_data[i] /= m_dt;
    }

    // Get size/nnz of spatial discretization and total for rows on this processor.
    // Allocate CSR structure.
    onProcSize  = m_ntPerProc * spatialDOFs;
    int procNnz = m_ntPerProc * (M_rowptr[spatialDOFs] + nnzPerTime);     // nnzs on this processor
    // Account for only identity at time t=0
    if (tInd0 == 0) procNnz -= (M_rowptr[spatialDOFs] + nnzPerTime - spatialDOFs);

    rowptr  = new int[onProcSize + 1];
    colinds = new int[procNnz];
    data = new double[procNnz];
    B = new double[onProcSize];
    X = new double[onProcSize];
    int dataInd = 0;
    int thisRow = 0;
    rowptr[0] = 0;

    // Loop over each time index and build sparse space-time matrix rows on this processor
    for (int ti=tInd0; ti<=tInd1; ti++) {

        // Build spatial matrix for next time step if coefficients are time dependent
        // (m_isTimeDependent) and matrix has not been built yet (ti > tInd0, ti > 1)
        if ((ti > tInd0) && m_isTimeDependent && (ti > 1)) {
            delete[] T_rowptr;
            delete[] T_colinds;
            delete[] T_data;
            delete[] B0;
            delete[] X0;
            getSpatialDiscretization(T_rowptr, T_colinds, T_data, B0,
                                     X0, spatialDOFs, m_dt*ti, m_bsize);
        }

        int colPlusOffd  = (ti - 1)*spatialDOFs;
        int colPlusDiag  = ti*spatialDOFs;
        std::map<int, double>::iterator it;

        // At time t=0, have fixed initial condition. Set matrix to identity
        // and fix RHS and initial guess to ICs.
        if (ti == 0) {
            
            // Add initial condition to rhs
            addInitialCondition(B);

            // Loop over each row in spatial discretization at time ti
            for (int i=0; i<spatialDOFs; i++) {

                colinds[dataInd] = i;
                data[dataInd] = 1.0;
                dataInd += 1;
                rowptr[thisRow+1] = dataInd;
                thisRow += 1;

                // Set solution equal to initial condition
                X[thisRow] = B[i];
            }
        }
        else {
            // Loop over each row in spatial discretization at time ti
            for (int i=0; i<spatialDOFs; i++) {
                std::map<int, double> entries;

                // Get row of mass matrix
                for (int j=M_rowptr[i]; j<M_rowptr[i+1]; j++) {                
                    entries[M_colinds[j]] -= M_data[j];
                }

                // Get row of spatial discretization
                for (int j=T_rowptr[i]; j<T_rowptr[i+1]; j++) {
                    entries[T_colinds[j]] += T_data[j];
                }

                // Add spatial discretization and mass matrix to global matrix
                // for off-diagonal block
                for (it=entries.begin(); it!=entries.end(); it++) {
                    if (std::abs(it->second) > 1e-16) {
                        colinds[dataInd] = colPlusOffd + it->first;
                        data[dataInd] = it->second;
                        dataInd += 1;
                    }
                }

                // Add mass matrix as diagonal block, M/dt
                for (int j=M_rowptr[i]; j<M_rowptr[i+1]; j++) {                
                    if (std::abs(M_data[j]) > 1e-16) {
                        colinds[dataInd] = colPlusDiag + M_colinds[j];
                        data[dataInd] = M_data[j];
                        dataInd += 1;
                    }
                }

                // Add right hand side and initial guess for this row to global problem
                B[thisRow] = B0[i];
                X[thisRow] = X0[i];

                // Total nonzero for this row on processor is one for off-diagonal block
                // and the total nnz in this row of T (T_rowptr[i+1] - T_rowptr[i]).
                rowptr[thisRow+1] = dataInd;
                thisRow += 1;
            }
        }

        // Check if sufficient data was allocated
        if (dataInd > procNnz) {
            std::cout << "WARNING: Matrix has more nonzeros than allocated.\n";
        }
    }
    delete[] T_rowptr;
    delete[] T_colinds;
    delete[] T_data;
    delete[] M_rowptr;
    delete[] M_colinds;
    delete[] M_data;
    delete[] B0;
    delete[] X0;
}


/* ------------------------------------------------------------------------- */
/* ------------------ At most one time step per processor ------------------ */
/* ------------------------------------------------------------------------- */

/* First-order BDF implicit scheme (Backward Euler / 1st-order Adams-Moulton). */
void SpaceTimeMatrix::BDF1(int* &rowptr, int* &colinds, double* &data,
                          double* &B, double* &X, int &localMinRow,
                          int &localMaxRow, int &spatialDOFs)
{
    // Get spatial discretization for first time step on this processor
    int* T_rowptr;
    int* T_colinds;
    double* T_data;
    getSpatialDiscretization(m_spatialComm, T_rowptr, T_colinds, T_data,
                             B, X, localMinRow, localMaxRow, spatialDOFs,
                             m_dt*m_timeInd, m_bsize);
    int procRows = localMaxRow - localMinRow + 1;
    int nnzPerTime = T_rowptr[procRows];    

    // Get mass matrix and scale by 1/dt
    int* M_rowptr;
    int* M_colinds;
    double* M_data;
    getMassMatrix(M_rowptr, M_colinds, M_data);
    for (int i=0; i<M_rowptr[procRows]; i++) {
        M_data[i] /= m_dt;
    }

    // Get number nnz on this processor.
    int procNnz;
    if (m_timeInd == 0) procNnz = procRows;
    else procNnz = nnzPerTime + (M_rowptr[procRows] - M_rowptr[0]);

    // Allocate CSR structure.
    rowptr  = new int[procRows + 1];
    colinds = new int[procNnz];
    data = new double[procNnz];
    int dataInd = 0;
    rowptr[0] = 0;

    // Column indices for this processor given the time index and the first row
    // stored on this processor of the spatial discretization (zero if whole
    // spatial problem stored on one processor).
    int colPlusOffd = (m_timeInd - 1)*spatialDOFs;
    int colPlusDiag = m_timeInd*spatialDOFs;
    std::map<int, double>::iterator it;
    // int colPlusOffd = (m_timeInd - 1)*spatialDOFs + localMinRow; 
    // OLD - I think this was incorrect -- Mass matrix and spatial matrix have
    //      column indices global w.r.t. the spatial discretization

    // At time t=0, have fixed initial condition. Set matrix to identity
    // and fix RHS and initial guess to ICs.
    if (m_timeInd == 0) {
        
        // Add initial condition to rhs
        // addInitialCondition(m_spatialComm, B);

        // Loop over each on-processor row in spatial discretization at time m_timeInd
        for (int i=0; i<procRows; i++) {

            colinds[dataInd] = localMinRow + i;
            data[dataInd] = 1.0;
            dataInd += 1;
            rowptr[i+1] = dataInd;
            i += 1;

            // Set solution equal to initial condition
            // X[i] = B[i];
            // X[i] = 0.0;
        }
    }
    else {
        // Loop over each row in spatial discretization at time m_timeInd
        for (int i=0; i<procRows; i++) {
            std::map<int, double> entries;

            // Add row of off-diagonal block, -(M/dt)*u_{i-1}
            for (int j=M_rowptr[i]; j<M_rowptr[i+1]; j++) {                
                if (std::abs(M_data[j]) > 1e-16) {
                    colinds[dataInd] = colPlusOffd + M_colinds[j];
                    data[dataInd] = -M_data[j];
                    dataInd += 1;
                    entries[M_colinds[j]] += M_data[j];
                }
            }

            // Get row of spatial discretization
            for (int j=T_rowptr[i]; j<T_rowptr[i+1]; j++) {
                entries[T_colinds[j]] += T_data[j];
            }

            // Add spatial discretization and mass matrix to global matrix
            for (it=entries.begin(); it!=entries.end(); it++) {
                if (std::abs(it->second) > 1e-16) {
                    colinds[dataInd] = colPlusDiag + it->first;
                    data[dataInd] = it->second;
                    dataInd += 1;
                }
            }

            // Total nonzero for this row on processor is one for off-diagonal block
            // and the total nnz in this row of T (T_rowptr[i+1] - T_rowptr[i]).
            rowptr[i+1] = dataInd;
        }
    }

    // Check if sufficient data was allocated
    if (dataInd > procNnz) {
        std::cout << "WARNING: Matrix has more nonzeros than allocated.\n";
    }
    delete[] T_rowptr;
    delete[] T_colinds;
    delete[] T_data;
    delete[] M_rowptr;
    delete[] M_colinds;
    delete[] M_data;
}


/* First-order Adams-Bashforth explicit scheme (Forward Euler). */
void SpaceTimeMatrix::AB1(int* &rowptr, int* &colinds,  double* &data,
                          double* &B, double* &X, int &localMinRow,
                          int &localMaxRow, int &spatialDOFs)
{
    // Get spatial discretization for first time step on this processor
    int* T_rowptr;
    int* T_colinds;
    double* T_data;
    if (m_timeInd == 0) {    
        getSpatialDiscretization(m_spatialComm, T_rowptr, T_colinds, T_data,
                                 B, X, localMinRow, localMaxRow, spatialDOFs,
                                 m_dt*m_timeInd, m_bsize);
    }
    else {
        getSpatialDiscretization(m_spatialComm, T_rowptr, T_colinds, T_data,
                                 B, X, localMinRow, localMaxRow, spatialDOFs,
                                 m_dt*(m_timeInd-1), m_bsize);
    }
    int procRows = localMaxRow - localMinRow + 1;
    int nnzPerTime = T_rowptr[procRows];    

    // Get mass matrix and scale by 1/dt
    int* M_rowptr;
    int* M_colinds;
    double* M_data;
    getMassMatrix(M_rowptr, M_colinds, M_data);
    for (int i=0; i<M_rowptr[procRows]; i++) {
        M_data[i] /= m_dt;
        // if (m_globRank == 3) std::cout << M_data[i] << ", ";
    }

    // if (m_globRank == 3) {
    //     std::cout << "\n\n";
    //     for (int i=0; i<T_rowptr[procRows]; i++) {
    //         std::cout << T_data[i] << ", ";
    //     }
    // }

    // Get size/nnz of spatial discretization and total for rows on this processor.
    // Allocate CSR structure.
    int procNnz;
    if (m_timeInd == 0) procNnz = spatialDOFs;
    else procNnz = nnzPerTime + (M_rowptr[procRows] - M_rowptr[0]);

    // Allocate CSR structure.
    rowptr  = new int[procRows + 1];
    colinds = new int[procNnz];
    data = new double[procNnz];
    int dataInd = 0;
    rowptr[0] = 0;

    // Local CSR matrices in off-diagonal blocks here have (spatially) global
    // column indices. Only need to account for min row indexing for the
    // diagonal block.
    int colPlusOffd = (m_timeInd - 1)*spatialDOFs;
    // int colPlusDiag = m_timeInd*spatialDOFs + localMinRow;
    // OLD - I think this was incorrect -- Mass matrix and spatial matrix have
    //      column indices global w.r.t. the spatial discretization
    int colPlusDiag = m_timeInd*spatialDOFs;
    std::map<int, double>::iterator it;

    // At time t=0, have fixed initial condition. Set matrix to identity
    // and fix RHS and initial guess to ICs.
    if (m_timeInd == 0) {
        
        // Add initial condition to rhs
        addInitialCondition(m_spatialComm, B);

        // Loop over each on-processor row in spatial discretization at time m_timeInd
        for (int i=0; i<procRows; i++) {

            colinds[dataInd] = localMinRow + i;
            data[dataInd] = 1.0;
            dataInd += 1;
            rowptr[i+1] = dataInd;
            i += 1;

            // Set solution equal to initial condition
            X[i] = B[i];
        }
    }
    else {
        // Loop over each row in spatial discretization at time ti
        for (int i=0; i<procRows; i++) {

            std::map<int, double> entries;

            // Get row of mass matrix
            for (int j=M_rowptr[i]; j<M_rowptr[i+1]; j++) {                
                entries[M_colinds[j]] -= M_data[j];
            }

            // Get row of spatial discretization
            for (int j=T_rowptr[i]; j<T_rowptr[i+1]; j++) {
                entries[T_colinds[j]] += T_data[j];
            }

            // Add spatial discretization and mass matrix to global matrix
            // for off-diagonal block
            for (it=entries.begin(); it!=entries.end(); it++) {
                if (std::abs(it->second) > 1e-16) {
                    colinds[dataInd] = colPlusOffd + it->first;
                    data[dataInd] = it->second;
                    dataInd += 1;
                }
            }

            // Add mass matrix as diagonal block, M/dt
            for (int j=M_rowptr[i]; j<M_rowptr[i+1]; j++) {                
                if (std::abs(M_data[j]) > 1e-16) {
                    colinds[dataInd] = colPlusDiag + M_colinds[j];
                    data[dataInd] = M_data[j];
                    dataInd += 1;
                }
            }

            // Total nonzero for this row on processor is one for off-diagonal block
            // and the total nnz in this row of T (T_rowptr[i+1] - T_rowptr[i]).
            rowptr[i+1] = dataInd;
        }
    }

    // Check if sufficient data was allocated
    if (dataInd > procNnz) {
        std::cout << "WARNING: Matrix has more nonzeros than allocated.\n";
    }
    delete[] T_rowptr;
    delete[] T_colinds;
    delete[] T_data;
    delete[] M_rowptr;
    delete[] M_colinds;
    delete[] M_data;
}

