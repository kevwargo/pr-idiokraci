CXX=mpic++
CXXFLAGS=-pthread -std=c++11

idiokracja.out: idiokracja.cpp
	$(CXX) $(CXXFLAGS) idiokracja.cpp -o idiokracja.out

single.out: single.cpp
	$(CXX) $(CXXFLAGS) single.cpp -o single.out
