# Test targets
# The actual testing is done via shell scripts

# Copy test scripts to build directory
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/generate_ground_truth.sh
               ${CMAKE_BINARY_DIR}/test/generate_ground_truth.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/run_ab_test.sh
               ${CMAKE_BINARY_DIR}/test/run_ab_test.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/compare_dot.py
               ${CMAKE_BINARY_DIR}/test/compare_dot.py COPYONLY)
