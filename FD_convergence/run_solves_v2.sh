#! /bin/bash

# Run the "driver" executable in ../ST_class for many different spatial refinements, nx.
# Then run the Python script "compute_errors.py" for each solve to compute the numerical errors.
# The solution information, including the errors, will be combined for all nx into one .npy file
# for which the data can be read and plotted.

# Notes:
#   -Run me like >> sh run_solves.sh
#   -For the data to be saved, you must have created a "data" subdirectory within the current directory.
#       Otherwise HYPRE will just ignore your request to output the data


#--- THE V2 HERE IS SINCE I COMPUTE DISC ERROR IN THE C++ CODE AND ITS STORED IN THE TEXT FILE... ---#

date

np=4            # Number of processes
FD=102            # ID of problem to be solved
min_refine=4    # Minimum spatial refinement
max_refine=11    # Maximum spatial refinement nx == 2^refine
pit=0           # Parallel-in-time flag
dim=1
rebuild=0       # Rate at which AMG solver is rebuilt in time-stepping
save=1          # Save only the text file output from the problem

# Choose one of these space+time pairs...
### --- Explicit Runge-Kutta --- ###
#time_type=RK
# space=1
# time=111
# space=2
# time=122
# space=3
# time=133
#space=4
#time=144

### --- Implicit Runge-Kutta --- ###
#time_type=RK
#space=1
#time=211
#space=2
#time=222
#space=3
#time=233
#space=4
#time=254

### --- BDF --- ###
time_type=BDF
#space=1
#time=31
#space=2
#time=32
#space=3
#time=33
#space=4
#time=34

# Name of file to be output... "^REFINE^" will be replaced with the actual refinement...
dir=data/"$time_type"
out="$time"_U"$space"_d"$dim"_l^REFINE^_FD"$FD"_pit"$pit"

# Run solves at different spatial refinements...
echo "solving..."
for l in `seq $min_refine $max_refine`
do
    echo "Spatial refinement = $l"

    # Run solver at current spatial refinement
    mpirun -np $np ./../ST_class/driver -s 3 -t $time -o $space -l $l -d $dim -FD $FD -pit $pit -p 3 -save $save -out $dir/U_${out/^REFINE^/$l} -tol 1e-8 -gmres 1 -maxit 40 -rebuild $rebuild -minit 1

done
