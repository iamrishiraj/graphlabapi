#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <graphlab.hpp>

#include <graphlab/macros_def.hpp>
#include "../../rapidjson.hpp"

#define INITIAL_LENGTH 256

namespace json = rapidjson;

double stof(const char *str){
  if (0 == strlen(str)) return 1e+99;
  return atof(str);
}

void shortest_path_update(json::Document & invocation, json::Document& return_json){

  json::Document::AllocatorType& allocator = return_json.GetAllocator();
  return_json.SetObject();

  // TODO: error handling for missing elements
  const char *vertex_state = invocation["params"]["context"]["vertex"]["state"].GetString();
  double vertex_dist = stof(vertex_state);
  
  // relax all incoming edges
  json::Value& in_edges = invocation["params"]["context"]["in_edges"];
  for (json::SizeType i = 0; i < in_edges.Size(); i++){
    const json::Value& edge = in_edges[i];
    double edge_dist = atof(edge["state"].GetString());
    double nbr_dist = stof(edge["source"]["state"].GetString());
    vertex_dist = std::min(vertex_dist, nbr_dist + edge_dist);
  }
  
  std::ostringstream double_stream;
  double_stream << vertex_dist << std::flush;
  const char *double_str = double_stream.str().c_str();
  
  // add shortest distance to return json
  json::Value vertex(double_str, allocator);
  return_json.AddMember("vertex", vertex, allocator);
  
  json::Value vertices;
  vertices.SetArray();
  
  // schedule affected members
  const json::Value& out_edges = invocation["params"]["context"]["out_edges"];
  for (json::SizeType i = 0; i < out_edges.Size(); i++){
  
    const json::Value& edge = out_edges[i];
    int nbr_id = edge["target"]["id"].GetInt();
    double nbr_dist = stof(edge["target"]["state"].GetString());
    double edge_dist = atof(edge["state"].GetString());
   
     if (nbr_dist > (vertex_dist + edge_dist))
       vertices.PushBack(nbr_id, allocator);
    
  }
  
  // add to return json
  json::Value schedule;
  schedule.SetObject();
  schedule.AddMember("updater", "self", allocator);
  schedule.AddMember("vertices", vertices, allocator);
  return_json.AddMember("schedule", schedule, allocator);

}

// not thread-safe
const char *handle_invocation(const char *buffer, json::StringBuffer& return_buffer){

  if (NULL == buffer) return NULL;

  json::Document invocation;
  if (invocation.Parse<0>(buffer).HasParseError()){/* TODO: error handling */}
  
  // TODO: error handling for missing elements
  if (!strcmp(invocation["method"].GetString(), "exit")) return NULL;
  if (!strcmp(invocation["method"].GetString(), "update")){
  
    json::Document return_json;
    shortest_path_update(invocation, return_json);
    
    return_buffer.Clear();
    json::Writer<json::StringBuffer> writer(return_buffer);
    return_json.Accept(writer);
    
    return return_buffer.GetString();
    
  }

  // TODO: error handling - method not found?
  return NULL;
  
}

int main(int argc, char** argv) {

  std::string line;
  std::size_t length = 0;
  std::size_t current_length = INITIAL_LENGTH;
  char *buffer = new char[current_length];
  
  // loop until exit is received
  while (true){
    
    // TODO: assume NULL-terminated string for now
    std::cin >> length;
    std::getline(std::cin, line);
    if (length + 1 > current_length){
      current_length = length + 1;
      delete[] buffer;
      buffer = new char[current_length];
    }
    
    // read message, break if exit
    std::cin.read(buffer, length);
    buffer[length] = NULL;    // terminate string w. null
    json::StringBuffer return_buffer;
    
    const char *return_json = handle_invocation(buffer, return_buffer);
    if (!return_json) break;
    
    // return
    std::cout << strlen(return_json) << "\n";
    std::cout << return_json << std::flush;
    
  }
  
  delete[] buffer;
  return 0;

}
