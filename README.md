test mpscfifo intrusive
===

Test Dmitry Vyukov's [intrusive mpsc fifo](http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue).

This does "work" if USE_RMV is 0 in test.c but doesn't if its 1. It appears
my stall routine just waiting on pTail->pNext to not be NULL is insufficient
I hope to someday figure out why.
