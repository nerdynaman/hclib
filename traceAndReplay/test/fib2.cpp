#include <iostream>
#include "hclib.hpp"

const int SIZE = 25165824;
const int THRESHOLD = 512;
// double A[SIZE + 2], A_shadow[SIZE + 2];
double *A;
double *A_shadow ;

void recurse(int low, int high) {
    if ((high - low) > THRESHOLD) {
        int mid = (high + low) / 2;
        // recurse(low, mid);
        // recurse(mid, high);
        hclib::finish([&]() {
            hclib::async([&]( ){recurse(low,mid);});
            recurse(mid,high);
	    });
    } else {
        for (int j = low; j < high; j++) {
            A_shadow[j] = (A[j - 1] + A[j + 1]) / 2.0;
        }
    }
}

void compute(int MAX_ITERS) {
    for (int i = 0; i < MAX_ITERS; i++) {
        hclib::start_tracing();
        recurse(1, SIZE + 1);
        std::swap(A,A_shadow);
        hclib::stop_tracing();
    }
}

int main(int argc, char ** argv) {
    hclib::init(argc, argv);
    // for (int i = 0; i < SIZE + 2; ++i) {
    //     A[i] = i;
    //     A_shadow[i] = 0; 
    // }
    A = new double[SIZE + 2];
    A_shadow = new double[SIZE + 2];
    A[SIZE+1] = 1.0;
    A_shadow[SIZE+1] = 1.0;
    // std::cout << "ARRAY IS:";
    // for (int i = 0; i < SIZE + 2; ++i) {
    //     std::cout << A[i] << " ";
    // }
    // std::cout << std::endl;

    hclib::kernel([&]() {
      compute(64);
    });
    // std::cout << "ANS ARRAY IS:";
    // for (int i = 0; i < SIZE + 2; ++i) {
    //     std::cout << A[i] << " ";
    // }
    // std::cout << std::endl;
    hclib::finalize();
    return 0;
}
