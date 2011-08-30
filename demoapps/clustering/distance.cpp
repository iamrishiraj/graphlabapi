#include "distance.h"
#include "clustering.h"
#include "../gabp/advanced_config.h"

extern advanced_config ac;

sparse_vec minus(sparse_vec & dvec1, sparse_vec & dvec2);
vec minus(sparse_vec & dvec1, vec & dvec2);
double sum_sqr(sparse_vec & dvec1);
sparse_vec fabs(sparse_vec& dvec1);
double sum(sparse_vec & dvec);

double calc_euclidian_distance( sparse_vec & datapoint,  sparse_vec &cluster){
  sparse_vec diff = minus(datapoint , cluster);
  return sqrt(sum_sqr(diff));
}


double calc_euclidian_distance( sparse_vec & datapoint,  vec &cluster){
  double dist = sum_sqr(cluster); //most of the entries of the datapoints are zero 
  for (int i=0; i< ((sparse_vec)datapoint).nnz(); i++){
      double val = ((sparse_vec)datapoint).get_nz_data(i);
      int pos = ((sparse_vec)datapoint).get_nz_index(i);
      dist += (((val - cluster[pos])*(val - cluster[pos])) - cluster[pos]*cluster[pos]);
   }
  return dist;
}

double calc_chebychev_distance( sparse_vec & datapoint,  sparse_vec &cluster){
   sparse_vec diff = minus(datapoint , cluster);
   double ret = 0;
   for (int i=0; i< diff.nnz(); i++)
      ret = std::max(ret, fabs(diff.get_nz_data(i)));

   return ret;

}
double calc_chebychev_distance( sparse_vec & datapoint,  vec &cluster){
   vec diff = minus(datapoint , cluster);
   double ret = 0;
   for (int i=0; i< diff.size(); i++)
      ret = std::max(ret, fabs(diff[i]));

   return ret;

}

double calc_manhatten_distance( sparse_vec & datapoint,  sparse_vec &cluster){
   sparse_vec diff = minus(datapoint , cluster);
   sparse_vec absvec = fabs(diff);
   double ret = sum(absvec);
   return ret;

}
double calc_manhatten_distance( sparse_vec & datapoint,  vec &cluster){
   vec diff = minus(datapoint , cluster);
   double ret = sum(abs(diff));
   return ret;

}

double calc_cosine_distance( sparse_vec & datapoint,  sparse_vec & cluster){
   double len_sqr1 = sum_sqr(datapoint);
   double len_sqr2 = sum_sqr(cluster);
   double dotprod = datapoint*cluster;
   double denominator = sqrt(len_sqr1)*sqrt(len_sqr2);
   return 1.0 - dotprod / denominator; 
}

double calc_cosine_distance( sparse_vec & datapoint,  vec & cluster){
   double len_sqr1 = sum_sqr(datapoint);
   double len_sqr2 = sum_sqr(cluster);
   double dotprod = datapoint*cluster;
   double denominator = sqrt(len_sqr1)*sqrt(len_sqr2);
   return 1.0 - dotprod / denominator; 
}


double calc_distance(itpp::sparse_vec &datapoint,  vec & cluster){
   switch(ac.distance_measure){
      case EUCLIDEAN:          
          return calc_euclidian_distance(datapoint, cluster);
      case CHEBYCHEV:
          return calc_chebychev_distance(datapoint, cluster);
      case COSINE:
	  return calc_cosine_distance(datapoint, cluster);  
      case MANHATTAN:
          return calc_manhatten_distance(datapoint, cluster);
      case MANAHOLIS:
      case WEIGHTED_MANAHOLIS:
      case WEIGHTED:
      default:
          logstream(LOG_ERROR)<< "distance measure " << ac.distance_measure<< "  not implemented yet" << std::endl;
    }
    return -1;

}


