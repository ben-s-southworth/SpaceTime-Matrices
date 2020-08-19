for i in {1..7};
do
	for j in {2..8}
	do
		# fixed temporal domain:
		mpirun -np $((2**i)) ./test -r $j -oU 2 -oP 1 -T 1 -P 1 -ST 0 -Pb 4 -V 0 -petscopts rc_SpaceTimeStokes
		# fixed dt:
		# mpirun -np $((2**i)) ./test -r $j -oU 2 -oP 1 -T $((2**(i-1))) -P 1 -ST 1 -Pb 2 -V 0 -petscopts rc_SpaceTimeStokes_approx2_fixdt
	done
done
