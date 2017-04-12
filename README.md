#  About

This is a fork of the Zero transactional manager. It is merged from work done by Min Xu (https://github.com/PickXu/zero-xum) and Caetano Sauer (https://github.com/caetanosauer/zero).  

It started as the merge of their master branches.  

I used it to run experiments on improving instant recovery for use in my BA thesis in Computer Science at the University of Chicago.

More information can be found in the 'README_old.md' in this repository.

# Additions

I added code to log the transaction page access information.  
Most of this code is in 'src/sm/log_spr.cpp'.  

(todo explain the generated files)

(todo add the options to turn off the loggging)

# Building

## Dependencies

On an Ubuntu system, the dependencies can usually be installed with the following commands:

```
sudo apt-get update
sudo apt-get install git cmake build-essential
sudo apt-get install libboost-dev libboost-thread-dev libboost-program-options-dev
sudo apt-get install libboost-random-dev libprotobuf-dev libzmq-dev libnuma-dev
sudo apt-get install libboost-all-dev
```

Zero requires libboost version 1.48. Please make sure that this version or a higher one is installed.

## Compilation

```
git clone https://github.com/PickXu/zero-xum.git
cd zero
mkdir build
```

To generate the build files, type:

```
cd build
cmake ..
```

Alternatively, a debug version without optimizations is also supported:

```
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

Finally, to compile:

```
make -j <number_of_cores> sm
make -j <number_of_cores> zapps
```

The `-j` flag enables compilation in parallel on multi-core CPUs. It is a standard feature of the Make building system. 
The `sm` target builds the static storage manager library, `libsm`.  
The 'zapps' target builds the execuatable we can use to run benchmarks on the sm.


# Guide on Running

Added sm options

("sm_xct_page_logging", po::value<bool>()->default_value(true),
     "Enable/Disable logging of xct page access")
("sm_single_page_logging", po::value<bool>()->default_value(true),
   "Enable/Disable logging of single page access")

("sm_startup_logging", po::value<bool>()->default_value(false),
   "Enable/Disable logging of sm startup timestamps")

("sm_recovery_logging", po::value<bool>()->default_value(true),
   "Enable/Disable logging of recovery access")
("sm_recovery_thread", po::value<bool>()->default_value(true),
  "Enable/Disable background recovery of pages")

(todo notes on running the executable)
