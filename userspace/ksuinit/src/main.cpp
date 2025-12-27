/**
 * ksuinit - KernelSU Init
 * 
 * This is the first userspace program to run before the real init.
 * It loads the KernelSU LKM module and then transfers control to the real init.
 * 
 * Rewritten from Rust to C++ for YukiSU.
 */

#include <unistd.h>
#include <cstdlib>

#include "init.hpp"

/**
 * Entry point - we use C main because we need to handle missing stdin/stdout/stderr
 * gracefully (Rust's std would abort in that case).
 */
int main(int argc, char* argv[], char* envp[]) {
    // Initialize KernelSU (mount filesystems, load LKM, setup init)
    ksuinit::init();
    
    // Transfer control to the real init
    execve("/init", argv, envp);
    
    // If execve fails, we're in trouble
    return 1;
}
