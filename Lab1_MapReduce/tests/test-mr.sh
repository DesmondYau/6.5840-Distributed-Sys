#!/bin/sh

#
# basic map-reduce test for C++ version
#

# run the test in a fresh sub-directory.
rm -rf mr-tmp
mkdir mr-tmp || exit 1
cd mr-tmp || exit 1
rm -f mr-*

# compile
(cd .. && g++-13 -std=c++23 mrsequential.cpp -o mrsequential -ldl) || exit 1
(cd ../../src && g++-13 -std=c++23 master.cpp -o master -lzmq -pthread ) || exit 1
(cd ../../src && g++-13 -std=c++23 worker.cpp -o worker -lzmq -ldl) || exit 1


failed_any=0

# ----------------------------------------------- 1. Word Count Test ----------------------------------------------------

# generate the correct output
../mrsequential ../wc.so ../pg*txt || exit 1
sort mr-out-0 > mr-correct-wc.txt
rm -f mr-out*

echo '***' Starting wc test.

../../src/master 1 ../pg*.txt &

# give the master time to create the sockets.
sleep 1

# start multiple workers.
../../src/worker ../wc.so &
../../src/worker ../wc.so &
../../src/worker ../wc.so &

wait

sort mr-out* | grep . > mr-wc-all

if cmp mr-wc-all mr-correct-wc.txt
then
  echo '---' wc test: PASS
else
  echo '---' wc output is not the same as mr-correct-wc.txt
  echo '---' wc test: FAIL
  failed_any=1
fi