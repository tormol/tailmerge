#!/bin/sh
set -e

# This script can be ran from `make test`

# empty or no values
./test_heap assert '' '' '' 0
./test_heap assert , '' 1 1

# 1-4
./test_heap assert = = 1 1
./test_heap assert x,y, x,y 1,2 2
./test_heap assert y,x, x,y 2,1 2
./test_heap assert x,y,z x,y,z 1,2,3 3
./test_heap assert x,z,y x,y,z 1,3,2 3
./test_heap assert y,x,z x,y,z 2,1,3 3
./test_heap assert y,z,x x,y,z 3,1,2 3
./test_heap assert z,x,y x,y,z 2,3,1 3
./test_heap assert z,y,x x,y,z 3,2,1 3
./test_heap assert a,b,c,d a,b,c,d 1,2,3,4 4
./test_heap assert d,c,b,a a,b,c,d 4,3,2,1 4

# stability
./test_heap assert ,, , 1,2 2
