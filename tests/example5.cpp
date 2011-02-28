#include <iostream>
#include <cstdio>
#include <graphlab/serialization/podify.hpp>
#include <graphlab/rpc/dc.hpp>
using namespace graphlab;


int main(int argc, char ** argv) {
  /** Initialization */
  size_t machineid = atoi(argv[1]);
  std::vector<std::string> machines;
  machines.push_back("127.0.0.1:10000");
  machines.push_back("127.0.0.1:10001");

  distributed_control dc(machines,"", machineid);
  
  if (dc.procid() == 0) {
    dc.remote_call(1, printf, "%d + %f = %s\n", 1, 2.0, "three");
  }
  getchar();
}
