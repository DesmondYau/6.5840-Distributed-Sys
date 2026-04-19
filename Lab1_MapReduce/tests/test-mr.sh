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
# (cd /6.5840/Lab1_MapReduce/build && cmake .. && make) || exit 1


failed_any=0

# ----------------------------------------------- 1. Word Count Test ----------------------------------------------------

# generate the correct output
../mrsequential ../wc.so ../pg*txt || exit 1
sort mr-out-0 > mr-correct-wc.txt
rm -f mr-out*

echo '***' Starting wc test.

../../build/master 1 ../pg*.txt &

# give the master time to create the sockets.
sleep 1

# start multiple workers.
../../build/worker ../wc.so &
../../build/worker ../wc.so &
../../build/worker ../wc.so &

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


# ----------------------------------------------- 2. Indexer Test ----------------------------------------------------
# wait for remaining workers and master to exit.
wait ; wait ; wait

# now indexer
rm -f mr-*

# generate the correct output
../mrsequential ../indexer.so ../pg*txt || exit 1
sort mr-out-0 > mr-correct-indexer.txt
rm -f mr-out*

echo '***' Starting indexer test.

../../build/master 8 ../pg*.txt &
sleep 1

# start multiple workers
../../build/worker ../indexer.so &
../../build/worker ../indexer.so &

wait

sort mr-out* | grep . > mr-indexer-all
if cmp mr-indexer-all mr-correct-indexer.txt
then
  echo '---' indexer test: PASS
else
  echo '---' indexer output is not the same as mr-correct-indexer.txt
  echo '---' indexer test: FAIL
  failed_any=1
fi


echo '***' Starting map parallelism test.
rm -f mr-out* mr-worker*

../../build/master 1 ../pg*.txt &
sleep 1

../../build/worker ../mtiming.so &
../../build/worker ../mtiming.so &

wait

NT=`cat mr-out* | grep '^times-' | wc -l | sed 's/ //g'`
if [ "$NT" != "2" ]
then
  echo '---' saw "$NT" workers rather than 2
  echo '---' map parallelism test: FAIL
  failed_any=1
else
  echo '---' map parallelism test: PASS
fi


# ----------------------------------------------- 3. Map Parallelism Test ----------------------------------------------------

echo '***' Starting map parallelism test.
rm -f mr-out* mr-worker*

../../build/master 8 ../pg*.txt &
sleep 1

../../build/worker ../mtiming.so &
../../build/worker ../mtiming.so &

wait

NT=`cat mr-out* | grep '^times-' | wc -l | sed 's/ //g'`
if [ "$NT" != "2" ]
then
  echo '---' saw "$NT" workers rather than 2
  echo '---' map parallelism test: FAIL
  failed_any=1
else
  echo '---' map parallelism test: PASS
fi


# ----------------------------------------------- 4. Reduce Parallelism Test ----------------------------------------------------

echo '***' Starting reduce parallelism test.
rm -f mr-out* mr-worker*

../../build/master 8 ../pg*.txt &
sleep 1

../../build/worker ../rtiming.so &
../../build/worker ../rtiming.so &

wait

NT=`cat mr-out* | grep '^[a-z] 2' | wc -l | sed 's/ //g'`
if [ "$NT" -lt "2" ]
then
  echo '---' too few parallel reduces.
  echo '---' reduce parallelism test: FAIL
  failed_any=1
else
  echo '---' reduce parallelism test: PASS
fi


# ----------------------------------------------- 5. Crash Test ----------------------------------------------------

echo '***' Starting crash test.

# generate the correct output
../mrsequential ../nocrash.so ../pg*txt || exit 1
sort mr-out-0 > mr-correct-crash.txt
rm -f mr-out

rm -f mr-done
(timeout -k 2s 180s ../../build/master 8 ../pg*txt ; touch mr-done ) &
sleep 1

# start multiple workers
timeout -k 2s 180s ../../build/worker ../crash.so &

# mimic rpc.go's masterSock()
# SOCKNAME=/var/tmp/824-mr-`id -u`

( while [ ! -f mr-done ]
  do
    timeout -k 2s 180s ../../build/worker ../crash.so
    sleep 1
  done ) &

( while [ ! -f mr-done ]
  do
    timeout -k 2s 180s ../../build/worker ../crash.so
    sleep 1
  done ) &

while [ ! -f mr-done ]
do
  timeout -k 2s 180s ../../build/worker ../crash.so
  sleep 1
done

wait
wait
wait

sort mr-out* | grep . > mr-crash-all
if cmp mr-crash-all mr-correct-crash.txt
then
  echo '---' crash test: PASS
else
  echo '---' crash output is not the same as mr-correct-crash.txt
  echo '---' crash test: FAIL
  failed_any=1
fi

if [ $failed_any -eq 0 ]; then
    echo '***' PASSED ALL TESTS
else
    echo '***' FAILED SOME TESTS
    exit 1
fi