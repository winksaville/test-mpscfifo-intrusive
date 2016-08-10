test mpscfifo intrusive
===

Test Dmitry Vyukov's [intrusive mpsc fifo](http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue).

This works with either USE_RMV 0 or 1, the problem I had with USE_RMV 1 was
I wasn't actually removing the last msg from the node when stalling, this
is now fixed.

I still need to implement the NON atomic types, USE_ATOMIC_TYPES 0.
