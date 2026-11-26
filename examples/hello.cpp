#include <alloy/datastructures/vector.hpp>
#include <alloy/datastructures/hash_map.hpp>
#include <alloy/core/hash_string.hpp>

#include <string>
#include <iostream>

using namespace alloy::ds;

int main() {
    hash_map<std::string, int> my_map;
    my_map["key"] = 42;
    std::cout << "Value: " << my_map["key"] << std::endl;
}
    