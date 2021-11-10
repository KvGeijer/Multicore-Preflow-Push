# Multicore-Preflow-Push
Some parallel implementations of the Preflow Push algorithm (Push Relabel). They were written for a competition for the most efficient implementation in the course EDAN26 at LTH by Jonas Skeppstedt.

Sadly I did not win, but learnt a ton about multicore programming and had a lot of fun. Our data set contained very many very simple graphs leading standard optimizations like concurrent global relabeling give negative speedups, which is why that branch is not merged to master. So the different branches are optimizations not deemed worthy of merging, but still having promise and being interesting.
