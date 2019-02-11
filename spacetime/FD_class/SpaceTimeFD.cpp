// #include <math.h>
#include <iostream>
#include <fstream>
#include <map>
#include <algorithm>
// #include "vis.c"
#include "SpaceTimeFD.hpp"


// Not sure if I have to initialize all of these to NULL. Do not do ParMatrix
// and ParVector because I think these are objects within StructMatrix/Vector. 
// 2d constructor
SpaceTimeFD::SpaceTimeFD(MPI_Comm comm, int nt, int nx, int Pt, int Px) :
    m_comm{comm}, m_nt_local{nt}, m_nx_local{nx}, m_Pt{Pt}, m_Px{Px},
    m_dim(2), m_rebuildSolver{false}, m_grid(NULL), m_graph(NULL),
    m_stencil_u(NULL), m_stencil_v(NULL), m_solver(NULL), m_gmres(NULL),
    m_bS(NULL), m_xS(NULL), m_AS(NULL)
{
    // Get number of processes
    MPI_Comm_rank(m_comm, &m_rank);
    MPI_Comm_size(m_comm, &m_numProc);

    if ((m_Px*m_Pt) != m_numProc) {
        if (m_rank == 0) {
            std::cout << "Error: Invalid number of processors or processor topology \n";
        }
        throw std::domain_error("Px*Pt != P");
    }

    // Compute local indices in 2d processor array, m_px_ind and
    // m_pt_ind, from m_Px, m_Pt, and m_rank
    m_n = m_nx_local * m_nt_local;
    m_px_ind = m_rank % m_Px;
    m_pt_ind = (m_rank - m_px_ind) / m_Px;

    // TODO : should be over (m_globx + 1)?? Example had this
    m_t0 = 0.0;
    m_t1 = 1.0;
    m_x0 = 0.0;
    m_x1 = 1.0;
    m_globt = m_nt_local * m_Pt;
    m_dt = (m_t1 - m_t0) / m_globt;
    m_globx = m_nx_local * m_Px;
    m_hx = (m_x1 - m_x0) / m_globx;
    m_hy = -1;

    // Define each processor's piece of the grid in space-time. Ordered by
    // time, then space
    m_ilower.resize(2);
    m_iupper.resize(2);
    m_ilower[0] = m_pt_ind * m_nt_local;
    m_iupper[0] = m_ilower[0] + m_nt_local-1;
    m_ilower[1] = m_px_ind * m_nx_local;
    m_iupper[1] = m_ilower[1] + m_nx_local-1;
}


SpaceTimeFD::~SpaceTimeFD()
{
    if (m_solver) HYPRE_BoomerAMGDestroy(m_solver);
    if (m_gmres) HYPRE_ParCSRGMRESDestroy(m_gmres);
    if (m_grid) HYPRE_SStructGridDestroy(m_grid);
    if (m_stencil_v) HYPRE_SStructStencilDestroy(m_stencil_v);
    if (m_stencil_u) HYPRE_SStructStencilDestroy(m_stencil_u);
    if (m_graph) HYPRE_SStructGraphDestroy(m_graph);
    if (m_AS) HYPRE_SStructMatrixDestroy(m_AS);     // This destroys parCSR matrix too
    if (m_bS) HYPRE_SStructVectorDestroy(m_bS);       // This destroys parVector too
    if (m_xS) HYPRE_SStructVectorDestroy(m_xS);
}


/* Set classical AMG parameters for BoomerAMG solve. */
void SpaceTimeFD::SetAMG()
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
void SpaceTimeFD::SetAIR()
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
void SpaceTimeFD::SetAIRHyperbolic()
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
void SpaceTimeFD::SetAMGParameters(AMG_parameters &params)
{
    // TODO: does this copy the structure by value?
    m_solverOptions = params;
}


void SpaceTimeFD::PrintMeshData()
{
    if (m_rank == 0) {
        std::cout << "Space-time mesh:\n\thx = " << m_hx <<
        "\n\thy = " << m_hy << "\n\tdt   = " << m_dt << "\n\n";
    }
}


/* Initialize AMG solver based on parameters in m_solverOptions struct. */
void SpaceTimeFD::SetupBoomerAMG(int printLevel, int maxiter, double tol)
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
            //HYPRE_BoomerAMGSetFilterThresholdR(m_solver, m_solverOptions.filter_tolR);
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


void SpaceTimeFD::SolveAMG(double tol, int maxiter, int printLevel)
{
    SetupBoomerAMG(printLevel, maxiter, tol);
    HYPRE_BoomerAMGSetup(m_solver, m_A, m_b, m_x);
    HYPRE_BoomerAMGSolve(m_solver, m_A, m_b, m_x);

    // Gather the solution vector
    HYPRE_SStructVectorGather(m_xS);
}


void SpaceTimeFD::SolveGMRES(double tol, int maxiter, int printLevel, int precondition) 
{
    HYPRE_ParCSRGMRESCreate(m_comm, &m_gmres);
    HYPRE_GMRESSetKDim(m_gmres, 5);
    HYPRE_GMRESSetMaxIter(m_gmres, maxiter);
    HYPRE_GMRESSetTol(m_gmres, tol);
    HYPRE_GMRESSetPrintLevel(m_gmres, printLevel);
    HYPRE_GMRESSetLogging(m_gmres, 1);

    // AMG preconditioning (setup boomerAMG with 1 max iter and print level 1)
    if (precondition == 1) {
        SetupBoomerAMG(1, 1, 0.0);
        HYPRE_ParCSRGMRESSetPrecond(m_gmres, HYPRE_BoomerAMGSolve,
                                    HYPRE_BoomerAMGSetup, m_solver);
    }
    // Diagonally scaled preconditioning?
    else if (precondition == 2) {
        int temp = -1;
    }
    // TODO: Implement block-diagonal AMG preconditioning, with BoomerAMG on each time block?
    else if (precondition == 3) {
        int temp = -1;
    }

    HYPRE_ParCSRGMRESSetup(m_gmres, m_A, m_b, m_x);
    HYPRE_ParCSRGMRESSolve(m_gmres, m_A, m_b, m_x);

    // Gather the solution vector
    HYPRE_SStructVectorGather(m_xS);
}


// First-order upwind discretization of the 1d-space, 1d-time homogeneous
// wave equation. Initial conditions (t = 0) are passed in function pointers
// IC_u(double x) and IC_v(double x), which give the ICs for u and v at point
// (0,x). 
void SpaceTimeFD::Wave1D(double (*IC_u)(double),
                         double (*IC_v)(double), 
                         double c)
{
    if (m_dim != 2) {
        if (m_rank == 0) {
            std::cout << "Error: class dimension is " << m_dim <<
                    ". This discretization requires dim=2.\n";
       }
       return;
    }

    if (m_rank == 0) {
        std::cout << "  1d Space-time wave equation:\n" <<
           "    (nx, nt) = (" << m_globx << ", " << m_globt << ")\n" << 
           "    (Px, Pt) = (" << m_Px << ", " << m_Pt << ")\n";
    }

    // Create an empty 2D grid object with 1 part
    HYPRE_SStructGridCreate(m_comm, m_dim, 1, &m_grid);

    // Add this processor's box to the grid (on part 0)
    HYPRE_SStructGridSetExtents(m_grid, 0, &m_ilower[0], &m_iupper[0]);

    // Define two variables on grid (on part 0)
    HYPRE_SStructVariable vartypes[2] = {HYPRE_SSTRUCT_VARIABLE_CELL,
                            HYPRE_SSTRUCT_VARIABLE_CELL };
    HYPRE_SStructGridSetVariables(m_grid, 0, 2, vartypes);

    // Finalize grid assembly.
    HYPRE_SStructGridAssemble(m_grid);

    /* ------------------------------------------------------------------
    *                   Define discretization stencils
    * ---------------------------------------------------------------- */
    // Stencil object for variable u (labeled as variable 0). First entry
    // is diagonal, next three are u at previous time, final three are v
    // at previous time
    std::vector<int>      uu_indices{0, 1, 2, 3};
    std::vector<int>      uv_indices{4, 5, 6};
    int n_uu_stenc      = uu_indices.size();
    int n_uv_stenc      = uv_indices.size();
    int stencil_size_u  = n_uu_stenc + n_uv_stenc;

    std::vector<std::vector<int> > offsets_u = {{0,0}, {-1,-1}, {-1,0}, {-1,1},
                                                {-1,-1}, {-1,0}, {-1,1}};
    HYPRE_SStructStencilCreate(m_dim, stencil_size_u, &m_stencil_u);

    // Data for stencil
    double lambda = c * m_dt / m_hx;
    std::vector<double> u_data = {1.0, lambda*lambda/2.0, 1-lambda*lambda,
                                  lambda*lambda/2.0, lambda*m_dt/4.0, 
                                  m_dt*(2.0-lambda)/2.0, lambda*m_dt/4.0 };
    std::vector<double> v_data = {1.0, lambda/2.0, 1.0-lambda, lambda/2.0,
                                  lambda*lambda/m_dt, -2.0*lambda*lambda/m_dt,
                                  lambda*lambda/m_dt};

    // Set stencil for u-u connections (variable 0)
    for (auto entry : uu_indices) {
        HYPRE_SStructStencilSetEntry(m_stencil_u, entry, &offsets_u[entry][0], 0);
    }

    // Set stencil for u-v connections (variable 1)
    for (auto entry : uv_indices) {
        HYPRE_SStructStencilSetEntry(m_stencil_u, entry, &offsets_u[entry][0], 1);
    }

    // Set u-stencil entries (to be added to matrix later). Note that
    // HYPRE_SStructMatrixSetBoxValues can only set values corresponding
    // to stencil entries for one variable at a time
    int n_uu = n_uu_stenc * m_n;
    double* uu_values = new double[n_uu];
    for (int i=0; i<n_uu; i+=n_uu_stenc) {
        for (int j=0; j<n_uu_stenc; j++) {
            uu_values[i+j] = u_data[j];
        }
    }

    // Fill in stencil for u-v entries here (to be added to matrix later)
    int n_uv = n_uv_stenc * m_n;
    double* uv_values = new double[n_uv];
    for (int i=0; i<n_uv; i+=n_uv_stenc) {
        for (int j=0; j<n_uv_stenc; j++) {
            uv_values[i+j] = u_data[j+n_uu_stenc];
        }
    }

    // Stencil object for variable v (labeled as variable 1).
    std::vector<int>     vv_indices{0, 1, 2, 3};
    std::vector<int>     vu_indices{4, 5, 6};
    int n_vv_stenc     = vv_indices.size();
    int n_vu_stenc     = vu_indices.size();
    int stencil_size_v = n_vv_stenc + n_vu_stenc;

    std::vector<std::vector<int> > offsets_v = {{0,0}, {-1,-1}, {-1,0}, {-1,1},
                                                {-1,-1}, {-1,0}, {-1,1}};
    HYPRE_SStructStencilCreate(m_dim, stencil_size_v, &m_stencil_v);

    // Set stencil for v-v connections (variable 1)
    for (auto entry : vv_indices) {
        HYPRE_SStructStencilSetEntry(m_stencil_v, entry, &offsets_v[entry][0], 1);
    }

    // Set stencil for v-u connections (variable 0)
    for (auto entry : vu_indices) {
        HYPRE_SStructStencilSetEntry(m_stencil_v, entry, &offsets_v[entry][0], 0);
    }

    // Set u-stencil entries (to be added to matrix later). Note that
    // HYPRE_SStructMatrixSetBoxValues can only set values corresponding
    // to stencil entries for one variable at a time
    int n_vv = n_vv_stenc * m_n;
    double* vv_values = new double[n_vv];
    for (int i=0; i<n_vv; i+=n_vv_stenc) {
        for (int j=0; j<n_vv_stenc; j++) {
            vv_values[i+j] = v_data[j];
        }
    }

    // Fill in stencil for u-v entries here (to be added to matrix later)
    int n_vu = n_vu_stenc * m_n;
    double* vu_values = new double[n_vu];
    for (int i=0; i<n_vu; i+=n_vu_stenc) {
        for (int j=0; j<n_vu_stenc; j++) {
            vu_values[i+j] = v_data[n_vv_stenc+j];
        }
    }

    /* ------------------------------------------------------------------
    *                      Fill in sparse matrix
    * ---------------------------------------------------------------- */

    HYPRE_SStructGraphCreate(m_comm, m_grid, &m_graph);
    HYPRE_SStructGraphSetObjectType(m_graph, HYPRE_PARCSR);
    HYPRE_SStructGraphSetObjectType(m_graph, HYPRE_PARCSR);

    // Assign the u-stencil to variable u (variable 0), and the v-stencil
    // variable v (variable 1), both on part 0 of the m_grid
    HYPRE_SStructGraphSetStencil(m_graph, 0, 0, m_stencil_u);
    HYPRE_SStructGraphSetStencil(m_graph, 0, 1, m_stencil_v);

    // Assemble the m_graph
    HYPRE_SStructGraphAssemble(m_graph);

    // Create an empty matrix object
    HYPRE_SStructMatrixCreate(m_comm, m_graph, &m_AS);
    HYPRE_SStructMatrixSetObjectType(m_AS, HYPRE_PARCSR);
    HYPRE_SStructMatrixInitialize(m_AS);

    // Set values in matrix for part 0 and variables 0 (u) and 1 (v)
    HYPRE_SStructMatrixSetBoxValues(m_AS, 0, &m_ilower[0], &m_iupper[0],
                                    0, n_uu_stenc, &uu_indices[0], uu_values);
    delete[] uu_values;
    HYPRE_SStructMatrixSetBoxValues(m_AS, 0, &m_ilower[0], &m_iupper[0],
                                    0, n_uv_stenc, &uv_indices[0], uv_values);
    delete[] uv_values;
    HYPRE_SStructMatrixSetBoxValues(m_AS, 0, &m_ilower[0], &m_iupper[0],
                                    1, n_vv_stenc, &vv_indices[0], vv_values);
    delete[] vv_values;
    HYPRE_SStructMatrixSetBoxValues(m_AS, 0, &m_ilower[0], &m_iupper[0],
                                    1, n_vu_stenc, &vu_indices[0], vu_values);
    delete[] vu_values;

    /* ------------------------------------------------------------------
    *                      Add boundary conditions
    * ---------------------------------------------------------------- */
    std::vector<int> periodic(m_dim,0);
    periodic[1] = m_globx;
    if ( (m_px_ind == 0) || (m_px_ind == (m_Px-1)) ) {
        HYPRE_SStructGridSetPeriodic(m_grid, 0, &periodic[0]);    
    }
    // TODO : do I set periodic on *all* processors, or only boundary processors??

    /* ------------------------------------------------------------------
    *                      Construct linear system
    * ---------------------------------------------------------------- */
    // Finalize matrix assembly
    HYPRE_SStructMatrixAssemble(m_AS);

    // Create an empty vector object
    HYPRE_SStructVectorCreate(m_comm, m_grid, &m_bS);
    HYPRE_SStructVectorCreate(m_comm, m_grid, &m_xS);

    // Set vectors to be par csr type
    HYPRE_SStructVectorSetObjectType(m_bS, HYPRE_PARCSR);
    HYPRE_SStructVectorSetObjectType(m_xS, HYPRE_PARCSR);

    // Indicate that vector coefficients are ready to be set
    HYPRE_SStructVectorInitialize(m_bS);
    HYPRE_SStructVectorInitialize(m_xS);

    // Set right hand side and inital guess. RHS is nonzero only at time t=0
    // because we are solving homogeneous wave equation. Because scheme is
    // explicit, set solution equal to rhs there because first t rows are
    // diagonal. Otherwise, rhs = 0 and we use 0 initial guess.
    //      TODO : should we be using zero initial guess for x0?
    std::vector<double> rhs(m_n, 0);
    if (m_pt_ind == 0) {
        for (int i=0; i<m_nx_local; i++) {
            double temp_x = (m_px_ind*m_nx_local + i) * m_hx;
            rhs[i] = IC_u(temp_x);
        }
    }
    HYPRE_SStructVectorSetBoxValues(m_bS, 0, &m_ilower[0], &m_iupper[0], 0, &rhs[0]);
    HYPRE_SStructVectorSetBoxValues(m_xS, 0, &m_ilower[0], &m_iupper[0], 0, &rhs[0]);

    if (m_pt_ind == 0) {
        for (int i=0; i<m_nx_local; i++) {
            double temp_x = (m_px_ind*m_nx_local + i) * m_hx;
            rhs[i] = IC_v(temp_x);
        }
    }
    HYPRE_SStructVectorSetBoxValues(m_bS, 0, &m_ilower[0], &m_iupper[0], 1, &rhs[0]);
    HYPRE_SStructVectorSetBoxValues(m_xS, 0, &m_ilower[0], &m_iupper[0], 1, &rhs[0]);

    // Finalize vector assembly
    HYPRE_SStructVectorAssemble(m_bS);
    HYPRE_SStructVectorAssemble(m_xS);

    // Get objects for sparse matrix and vectors.
    HYPRE_SStructMatrixGetObject(m_AS, (void **) &m_A);
    HYPRE_SStructVectorGetObject(m_bS, (void **) &m_b);
    HYPRE_SStructVectorGetObject(m_xS, (void **) &m_x);
//#endif
}


#if 0
// Save the solution for GLVis visualization, see vis/glvis-ex7.sh
// TODO : Fix, add numvariables as option? 
SpaceTimeFD::VisualizeSol()
{
    FILE *file;
    char filename[255];

    int k, part = 0, var;
    int m_n = n*n;
    double *values = (double*) calloc(m_n, sizeof(double));

    /* save local solution for variable u */
    var = 0;
    HYPRE_SStructVectorGetBoxValues(m_xS, 0, m_ilower, m_iupper,
                                    var, values);

    sprintf(filename, "%s.%06d", "vis/ex9-u.sol", m_rank);
    if ((file = fopen(filename, "w")) == NULL) {
        printf("Error: can't open output file %s\n", filename);
        MPI_Finalize();
        exit(1);
    }

    /* save solution with global unknown numbers */
    int k = 0;
    for (int j=0; j<n; j++) {
        for (int i=0; i<n; i++){
            fprintf(file, "%06d %.14e\n", pj*N*n*n+pi*n+j*N*n+i, values[k++]);
        }
    }

    fflush(file);
    fclose(file);

    /* save local solution for variable v */
    var = 1;
    HYPRE_SStructVectorGetBoxValues(m_xS, 0, m_ilower, m_iupper,
                                    var, values);

    sprintf(filename, "%s.%06d", "vis/ex9-v.sol", m_rank);
    if ((file = fopen(filename, "w")) == NULL) {
        printf("Error: can't open output file %s\n", filename);
        MPI_Finalize();
        exit(1);
    }

    /* save solution with global unknown numbers */
    k = 0;
    for (int j=0; j<n; j++) {
        for (int i=0; i<n; i++) {
            fprintf(file, "%06d %.14e\n", pj*N*n*n+pi*n+j*N*n+i, values[k++]);
        }
    }

    fflush(file);
    fclose(file);

    free(values);

    /* save global finite element mesh */
    if (m_rank == 0){
        GLVis_PrintGlobalSquareMesh("vis/ex9.mesh", N*n-1);
    }
}
#endif