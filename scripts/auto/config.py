#!/usr/bin/env python3

from auto.utils import *

import os

### config ###

# flags
CFLAGS          = "-fno-exceptions"
CLANG_CFLAGS    = "-fno-rtti"
_O              = "-O3"

emit_llvm       = lambda dbg, opt: \
    "-emit-llvm -c {}{} -mllvm -disable-llvm-optzns".format(
        "-g " if dbg else "", _O if opt else "-O0")
RV32_TRIPLE     = "riscv32-unknown-elf"


class Dir:
    def __init__(self):
        top             = os.environ["TOPDIR"]
        build_type      = os.getenv("BUILD_TYPE", "Debug")
        build_type_dir  = build_type.lower()
        self.build_type_dir = build_type_dir
        toolchain       = top + "/toolchain"

        self.top                = top
        self.log                = top + "/junk"
        self.toolchain_release  = toolchain + "/release"
        self.toolchain_debug    = toolchain + "/debug"
        self.toolchain          = toolchain + "/" + build_type_dir
        self.remote             = top + "/remote"
        self.build              = top + "/build"
        self.patches            = top + "/patches"
        self.scripts            = top + "/scripts"
        self.auto               = self.scripts + "/auto"
        self.submodules         = top + "/submodules"

DIR = Dir()


class Sbt:
    def __init__(self):
        self.flags = "-debug"
        #self.flags = cat(self.flags,
        #        "-enable-fcsr",
        #        "-enable-fcvt-validation")
        self.share_dir = DIR.toolchain + "/share/riscv-sbt"
        self.modes = ["globals", "locals"]

    def nat_obj(self, arch, name, clink):
        if arch == RV32 or arch == RV32_LINUX and name != "runtime":
            return ""
        if name == "runtime" and not clink:
            return ""
        return "{}/{}-{}.o".format(self.share_dir, arch.prefix, name)


SBT = Sbt()


class Tools:
    def __init__(self):
        self.cmake      = "cmake"
        self.opt        = "opt"
        self.opt_flags  = lambda opt: _O if opt else "-O0"
        self.dis        = "llvm-dis"
        self.link       = "llvm-link"
        self.mc         = "llvm-mc"
        self.objdump    = "riscv64-unknown-linux-gnu-objdump"

        self.build      = path(DIR.auto, "build.py")
        self.run        = path(DIR.auto, "run.py")
        self.xlate      = path(DIR.auto, "xlate.py")
        self.measure    = path(DIR.auto, "measure.py")

TOOLS = Tools()


class Arch:
    def __init__(self, name, prefix, triple, run, march, gcc_flags,
            clang_flags, sysroot, isysroot, llc_flags, as_flags,
            ld_flags, mattr):

        self.name = name
        self.prefix = prefix
        self.triple = triple
        self.run = run
        self.march = march
        # gcc
        self.gcc = triple + "-gcc"
        self.gcc_flags = lambda dbg, opt: \
            "{}{} {} {}".format(
                ("-g " if dbg else ""),
                (_O if opt else "-O0"),
                CFLAGS,
                gcc_flags)
        # clang
        self.clang = "clang"
        self.clang_flags = "{} {}".format(CFLAGS, clang_flags)
        self.sysroot = sysroot
        self.isysroot = isysroot
        self.sysroot_flag = "-isysroot {} -isystem {}".format(
            sysroot, isysroot)
        # llc
        self.llc = "llc"
        self.llc_flags = lambda opt: \
            cat("-relocation-model=static",
                (_O if opt else "-O0"),
                llc_flags)
        # as
        self._as = triple + "-as"
        self.as_flags = as_flags
        # ld
        self.ld = triple + "-ld"
        self.ld_flags = ld_flags
        #
        self.mattr = mattr


    def add_prefix(self, s):
        return self.prefix + "-" + s


    def src2objname(self, src):
        """ objname for source code input file """
        if src.startswith(self.prefix):
            name = src
        else:
            name = self.add_prefix(src)
        return chsuf(name, ".o")


    def out2objname(self, out):
        """ objname for output file """
        if out.startswith(self.prefix):
            name = out
        else:
            name = self.add_prefix(out)
        return name + ".o"



PK32 = DIR.toolchain_release + "/" + RV32_TRIPLE + "/bin/pk"
RV32_SYSROOT    = DIR.toolchain_release + "/opt/riscv/" + RV32_TRIPLE
RV32_MARCH      = "riscv32"
RV32_MATTR      = "-a,-c,+m,+f,+d"
RV32_LLC_FLAGS  = cat("-march=" + RV32_MARCH, "-mattr=" + RV32_MATTR)

RV32 = Arch(
        name="rv32",
        prefix="rv32",
        triple=RV32_TRIPLE,
        run="LD_LIBRARY_PATH={}/lib spike {}".format(
            DIR.toolchain_release, PK32),
        march=RV32_MARCH,
        gcc_flags="",
        clang_flags=cat(CLANG_CFLAGS, "--target=riscv32"),
        sysroot=RV32_SYSROOT,
        isysroot=RV32_SYSROOT + "/include",
        llc_flags=RV32_LLC_FLAGS,
        as_flags="",
        ld_flags="",
        mattr=RV32_MATTR)


RV32_LINUX_SYSROOT  = DIR.toolchain_release + "/opt/riscv/sysroot"
RV32_LINUX_ABI      = "ilp32"
RV32_LINUX_GCC_FLAGS    = "-march=rv32g -mabi=" + RV32_LINUX_ABI
RV32_LINUX_AS_FLAGS     = "-march=rv32g -mabi=" + RV32_LINUX_ABI
RV32_LINUX_LD_FLAGS     = "-m elf32lriscv"

RV32_LINUX = Arch(
        name="rv32-linux",
        prefix="rv32",
        triple="riscv64-unknown-linux-gnu",
        run="qemu-riscv32 -L " + RV32_LINUX_SYSROOT,
        march=RV32_MARCH,
        gcc_flags=RV32_LINUX_GCC_FLAGS,
        clang_flags=cat(CLANG_CFLAGS, "--target=riscv32 -D__riscv_xlen=32"),
        sysroot=RV32_LINUX_SYSROOT,
        isysroot=RV32_LINUX_SYSROOT + "/usr/include",
        llc_flags=RV32_LLC_FLAGS,
        as_flags=RV32_LINUX_AS_FLAGS,
        ld_flags=RV32_LINUX_LD_FLAGS,
        mattr=RV32_MATTR)


X86_MARCH       = "x86"
X86_MATTR       = "avx"
X86_SYSROOT     = "/"
X86_ISYSROOT    = "/usr/include"

X86 = Arch(
        name="x86",
        prefix="x86",
        triple="x86_64-linux-gnu",
        run="",
        march=X86_MARCH,
        gcc_flags="-m32",
        clang_flags=cat(CLANG_CFLAGS,
            "--target=x86_64-unknown-linux-gnu -m32"),
        sysroot=X86_SYSROOT,
        isysroot=X86_ISYSROOT,
        llc_flags=cat("-march=" + X86_MARCH, "-mattr=" + X86_MATTR),
        as_flags="--32",
        ld_flags="-m elf_i386",
        mattr=X86_MATTR)


RV32_FOR_X86 = Arch(
        name="rv32-for-x86",
        prefix="rv32-for-x86",
        triple=RV32_LINUX.triple,
        run="",
        march=RV32_MARCH,
        gcc_flags=RV32_LINUX_GCC_FLAGS,
        clang_flags=cat(CLANG_CFLAGS, "--target=riscv32"),
        sysroot=X86_SYSROOT,
        isysroot=X86_ISYSROOT,
        llc_flags=RV32_LLC_FLAGS,
        as_flags=RV32_LINUX_AS_FLAGS,
        ld_flags=RV32_LINUX_LD_FLAGS,
        mattr=RV32_MATTR)


# arch map
ARCH = {
    "rv32"          : RV32,
    "rv32-linux"    : RV32_LINUX,
    "x86"           : X86,
    "rv32-for-x86"  : RV32_FOR_X86,
}

###
