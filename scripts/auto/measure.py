#!/usr/bin/python3

from auto.config import *
from auto.utils import path

import argparse
import math
import numpy as np
import os
import subprocess
import sys
import time

class Options:
    def __init__(self, stdin, verbose, exp_rc):
        self.stdin = stdin
        self.verbose = verbose
        self.exp_rc = exp_rc


class Program:
    def __init__(self, dir, basename, arch, mode, args, opts):
        self.name = arch + '-' + basename
        if mode:
            self.name = self.name + '-' + mode
        else:
            mode = 'native'
        self.mode = mode
        self.args = [path(dir, self.name)] + args
        self.opts = opts
        self.times = []

        if self.opts.verbose:
            print(" ".join(self.args))


    def _out(self, i):
        return "/tmp/" + self.name + str(i) + ".out"


    def _check_rc(self, rc):
        exp_rc = self.opts.exp_rc
        if rc != exp_rc:
            raise Exception("Failure! rc=" + str(rc) + " exp_rc=" + str(exp_rc))


    def run(self, i):
        with open(self._out(i), 'wb') as fout:
            stdin = self.opts.stdin
            if stdin:
                with open(stdin, 'rb') as fin:
                    t0 = time.time()
                    cp = subprocess.call(self.args, stdin=fin, stdout=fout)
                    t1 = time.time()
                    self._check_rc(cp)
            else:
                t0 = time.time()
                cp = subprocess.call(self.args, stdout=fout)
                t1 = time.time()
                self._check_rc(cp)
        t = t1 - t0
        self.times.append(t)
        if self.opts.verbose:
            print("run #" + str(i) + ": time taken:", t)
        sys.stdout.flush()


class Measure:
    def __init__(self, dir, prog, args, opts):
        self.dir = dir
        self.prog = prog
        self.args = args
        self.opts = opts


    def measure(self):
        for target in ['x86']:
            self._measure_target(target)


    def _measure_target(self, target):
        # prepare programs
        nprog = Program(self.dir, self.prog, target, None, self.args, self.opts)
        xarch = 'rv32-' + target
        xprogs = [
            Program(self.dir, self.prog, xarch, mode, self.args, self.opts)
                for mode in SBT.modes]

        # run
        N = 10
        times = {}
        for prog in [nprog] + xprogs:
            if self.opts.verbose:
                print("measuring", prog.name)
            for i in range(N):
                prog.run(i)
            times[prog.mode] = prog.times

        # get means
        nat_m = None
        nat_sd = None
        for mode in ['native'] + SBT.modes:
            m = self.mean(times[mode])
            sd = self.sd(times[mode], m)
            if mode == 'native':
                nat_m = m
                nat_sd = sd
            slowdown = 1 + (m - nat_m) / nat_m
            print("{0:<8}: {1:.5f} {2:.5f} {3:.2f}".
                    format(mode, m, sd, slowdown))


    @staticmethod
    def mean(vals):
        return sum(vals) / len(vals)


    @staticmethod
    def sd(vals, mean):
        return math.sqrt(
            sum([math.pow(val - mean, 2) for val in vals])
            / (len(vals) - 1))


    @staticmethod
    def median(vals):
        vals.sort()
        n = len(vals)
        if n % 2 == 0:
            v1 = vals[n//2 -1]
            v2 = vals[n//2]
            return (v1 + v2) / 2
        else:
            return vals[n//2 +1]


    @staticmethod
    def geomean(iterable):
        a = np.array(iterable)
        return a.prod()**(1.0/len(a))



# dir test arg
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Measure translation overhead')
    parser.add_argument('dir', type=str)
    parser.add_argument('test', type=str)
    parser.add_argument("--args", nargs="*", metavar="arg", default=[])
    parser.add_argument("--stdin", help="stdin redirection")
    parser.add_argument("-v", action="store_true", help="verbose")
    parser.add_argument("--exp-rc", type=int, default=0,
        help="expected return code")

    args = parser.parse_args()
    sargs = [arg.strip() for arg in args.args]

    # args.v = True
    opts = Options(args.stdin, args.v, args.exp_rc)
    measure = Measure(args.dir, args.test, sargs, opts)
    measure.measure()

"""
    ### main_factor_libc_out ###

    RUNSCENARIO="run_test_factor_libc_out"
    CHECKOUTPUT=0
    OUTSCENARIO="$REMOTEINSTALL/mibench_factor_libc_out.csv"
    echo -ne "Index Program Native NStdDev Globals GStdDev Locals LStdDev Whole WStdDev Abi AStdDev\n" | tee $OUTSCENARIO
    run_mibench

    # run_family
    #
    # run_test_factor_libc_out:
    # repeat 10:
    #   perf record -q {bin}
    #   perf report --sort=dso > perfreport.txt
    #   extract_info ${progname} "perfreport.txt"
    #   # RET=<percentage of time spent on the binary>
    # calculate mean and stddev
    # means[$progname]=$mean
    # stddevs[$progname]=$sd
    # echo -ne "$mean " | tee -a mibench_factor_libc_out.csv
    # echo -ne "$sd" | tee -a mibench_factor_libc_out.csv

    ### main_measure_runtime ###
    RUNSCENARIO="run_test_measure_time"
    CHECKOUTPUT=0
    OUTSCENARIO="$REMOTEINSTALL/mibench_runtime.csv"
    echo -ne "Index Program Globals GError Locals LError Whole WError Abi AError\n" | tee $OUTSCENARIO
    run_mibench

    # run_family
    #
    # run_test_measure_time
    # repeat 10:
    #   perf stat {bin}
    #   extract_perf_time # get elapsed time
    #
    # time_mean
    # time_sd
    # factored_mean = ${means[$progname]}
    # factored_sd = ${stddevs[$progname]}
    #
    # mean = time_mean * factored_mean
    # sd = mean * sqrt((time_sd / time_mean)^2 + (factored_sd / factored_mean)^2)
    # # sqrt(...) -> error propagation
    #
    # mean = mean / NATMEAN
    # echo mean
    # sd = mean * sqrt((sd / mean)^2 + (NATSD / NATMEAN)^2)
    # echo sd
"""
