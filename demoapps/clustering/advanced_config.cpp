#include "clustering.h"

#include "../gabp/advanced_config.h"

extern problem_setup ps;
extern advanced_config ac;

void advanced_config::init_command_line_options(graphlab::command_line_options & clopts){

  clopts.attach_option("datafile", &datafile, datafile, "Input matrix/tensor");
  clopts.add_positional("datafile");

  clopts.attach_option("algorithm", &algorithm, algorithm, "ps.algorithm");
  clopts.add_positional("algorithm");
  
  clopts.attach_option("debug", &debug, debug, "Display debug output. (optional)");
  clopts.attach_option("D", &K, K, "number of clusters");
  clopts.attach_option("zero", &zero, zero, "support zero edges");  
  clopts.attach_option("savegraph", &savegraph, savegraph, "save graphs to file");  
  clopts.attach_option("loadgraph", &loadgraph, loadgraph, "load graphs to file");  
  clopts.attach_option("savefactors", &savefactors, savefactors, "save factors to file");  
  clopts.attach_option("loadfactors", &loadfactors, loadfactors, "load factors from file");  
  clopts.attach_option("stats", &stats, stats, "compute graph statistics");  
  clopts.attach_option("binaryoutput", &binaryoutput, binaryoutput, "export U,V,T to a binary file"); 
  clopts.attach_option("matrixmarket", &matrixmarket, matrixmarket, "give input in matrix market format"); 
 
  clopts.attach_option("scalerating", &scalerating, scalerating, "scale data by this factor");  
  clopts.attach_option("maxval", &maxval, maxval, "maximal allowed value in data");
  clopts.attach_option("minval", &minval, minval, "minimal allowed value in data");
  clopts.attach_option("unittest", &unittest, unittest, "unit testing. ");
 
  //SVD related switches
  clopts.attach_option("printhighprecision", &printhighprecision, printhighprecision, "print RMSE output with high precision");

}

void problem_setup::verify_setup(){

   assert(K>0);
}
