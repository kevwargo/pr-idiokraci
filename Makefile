CXX=mpic++
CXXFLAGS=-pthread -std=c++11

single.out: single.cpp
	$(CXX) $(CXXFLAGS) single.cpp -o single.out
