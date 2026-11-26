#include <alloy/datastructures/vector.hpp>
#include <alloy/datastructures/hash_map.hpp>
#include <alloy/core/hash_string.hpp>
#include <alloy/datastructures/vector.hpp>

#include <print>
#include <string>
#include <iostream>

using namespace alloy::ds;

auto main() -> int {
    hash_map<std::string, int> my_map;
    my_map["key"] = 42;
    std::cout << "Value: " << my_map["key"] << '\n';

    vector<int32_t> my_vec;
    for (int32_t i = 0; i < 10000; ++i) {
        my_vec.push_back(i);
    }

    for (size_t i = 0; i < my_vec.size(); ++i) {
        std::println("{}", my_vec[i]);
    }
}
    