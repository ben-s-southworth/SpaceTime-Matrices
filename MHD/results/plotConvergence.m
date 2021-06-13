function plotConvergence( xord )

if (nargin<1 || isempty(xord))
	xord = 2;
end

err   = cell(5,1);
names = {'u','p','z','A','tot'};
err{1} = load('solutionConvergenceU.dat');
err{2} = load('solutionConvergenceP.dat');
err{3} = load('solutionConvergenceZ.dat');
err{4} = load('solutionConvergenceA.dat');
err{5} = load('solutionConvergence.dat');

figure
for i = 1:length(err)
  subplot(3,length(err),i);
  semilogy(err{i});
  hold on
  semilogy(2.^(-xord*(1:size(err{i},1))),'k*--')
  ylabel(strcat('err',names{i}));
  title('dx convergence');
  hold off
  
  subplot(3,length(err),i+length(err));
  semilogy(err{i}');
  hold on
  semilogy(2.^(-2*(1:size(err{i},2))),'k*--')
  ylabel(strcat('err',names{i}));
  title('dt convergence');
  
  subplot(3,length(err),i+2*length(err));
  surf(err{i});
  ylabel('dx');
  xlabel('dt');
  title(strcat('err',names{i}));
  h = gca;
  set(h,'zscale','log')
  
end