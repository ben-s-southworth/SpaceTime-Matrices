function [ x ] = fakePrecon( b, Fu, Mu, B, Ap, Mp, Wp, essIdx )

% useful info
nu = size(B,2);
np = size(B,1);
NT = length(b) / (nu+np);
dt = 1./NT;
mu = 1.;

% reorganise rhs (a column corresponds to a specific instant)
g = reshape( b( (nu*NT+1):end ), [np,NT] );
f = reshape( b( 1:(nu*NT)     ), [nu,NT] );


% Apply pressure schur complement approximation
% - solve for pressure "laplacian" at each instant
if isempty(essIdx)					% if empty, then Ap is singular
	invAx = zeros(size(g));		% so arbitrarily fix the first component to zero
	invAx(2:end,:) = Ap(2:end,2:end)\g(2:end,:);
else
	invAx = Ap\g;
end
% - include contribution from spatial operator
% if isempty(essIdx)					% again, if empty, also modify Wp accordingly
% 	Wp(1,:) = sparse(1,size(Wp,2));
% 	Wp(:,1) = sparse(size(Wp,1),1);
% 	Wp(1,1) = 1;
% end
lclx  = mu*dt* Wp * invAx;
% - solve for pressure mass matrix at each instant
invMx = Mp\lclx;
% - combine the two contributions
solp  = invMx + invAx;
% - include contribution from previous time-step
solp(:,2:end) = solp(:,2:end) - invAx(:,1:end-1);
solp = - solp;	% remember it's -XX^-1 in bottom-right


% Apply top-right block
f = f - B'*solp;


% Apply Velocity time-stepping
solu  = zeros(nu,NT);
uprev = zeros(nu,1);
for i=1:NT
	solu(:,i) = Fu \ ( f(:,i) - Mu*uprev );
	uprev = solu(:,i);
end


% - rearrange solutions in a single vector
p = solp(:);
u = solu(:);

x = [u;p];

end

