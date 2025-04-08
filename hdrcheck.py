# Tested like:
# make O=build-allno LLVM=1 LLVM_SUFFIX=-19 allnoconfig
# make O=build-allno LLVM=1 LLVM_SUFFIX=-19 -s
# python3 ./hdrcheck.py  build-allno/ 2>&1 > hdrcheck
# ninja -f hdrcheck -k0 &> log

import collections
import os
import sys
import re

exclude_headers = [
    r"include/asm-.*",
    r"include/acpi/.*",
    "include/linux/compiler-version.h",
    "arch/x86/events/perf_event_flags.h",
    "arch/x86/.*",
    r".*/crc32table.h",
    r".*/generated/.*",
    "drivers/tty/tty.h",
]

# Represents a single .o.cmd savedcmd_
class CC:
    count = 0
    rule_id = None

    def __init__(self, cur_dir, cc):
        cc = re.sub(r"-Wp,-MMD,\S+ ", "", cc)
        cc = re.sub(r"-DKBUILD_MODFILE='.+?' ", "", cc)
        cc = re.sub(r"-DKBUILD_BASENAME='.+?' ", "", cc)
        cc = re.sub(r"-DKBUILD_MODNAME='.+?' ", "-DKBUILD_MODNAME='\"hdcheck\"' ", cc)
        cc = re.sub(r"-D__KBUILD_MODNAME=\S+ ", "", cc)
        cc = re.sub(r"-o \S+ ", "", cc)
        cc = re.sub(r" \S+.c$", "", cc)
        self.cc = cc
        self.cur_dir = cur_dir

    def __eq__(self, other):
        return other and self.cc == other.cc and self.cur_dir == other.cur_dir

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((self.cc, self.cur_dir))


all_ccs = {}


# Parse the .XX.o.cmd file that kbuild leaves behind to learn the list of
# header files and the compiler command
def parse_o_cmd(build_dir, fn, hdrs):
    cur_dir = os.path.relpath(os.path.dirname(fn), build_dir)
    with open(fn, "rt") as F:
        lines = iter(F.readlines())
        for ln in lines:
            if ln.startswith("savedcmd_"):
                cc = ln.partition(" := ")[2]
                cc = cc.strip()
                if not cc.endswith(".c"):
                    return
                cc = CC(cur_dir, cc)
                if cc in all_ccs:
                    cc = all_ccs[cc]
                else:
                    all_ccs[cc] = cc

            if ln.startswith("deps_"):
                break

        for ln in lines:
            ln = ln.strip()
            if not ln:
                break
            if ln.endswith(".h \\"):
                ln = ln[:-2]
            if ln.endswith(".h"):
                hdr = os.path.join(build_dir, ln)
                hdr = os.path.normpath(hdr)
                cc.count += 1
                hdrs[hdr].add(cc)


build_dir = sys.argv[1]

# Parse all .XX.o.cmd files
hdrs = collections.defaultdict(set)
for dirpath, dirnames, filenames in os.walk(build_dir):
    for I in filenames:
        if I.startswith(".") and I.endswith(".o.cmd"):
            parse_o_cmd(build_dir, os.path.join(dirpath, I), hdrs)

for k in list(hdrs.keys()):
    for I in exclude_headers:
        if re.match(I, k) is not None:
            del hdrs[k]
            break

# Write out ninja build rules for CC commands for each unique compiler
# There are many compiler options for each of the header files, choose the
# most common to minimize the number of rules.
print(f"builddir={os.path.realpath(build_dir)}")
print(f"srcdir={os.path.realpath('.')}")
rule_id = 0
for k in sorted(hdrs.keys()):
    for cc in sorted(hdrs[k], key=lambda cc: cc.count):
        if cc.rule_id:
            break
        print(f"rule hdrcheck_{rule_id}")
        print(f" command=cd $builddir && {cc.cc} -o $out $in")
        print(f" description=$in")
        cc.rule_id = rule_id
        rule_id += 1
        break

# Write out the ninja build build lines
for k in sorted(hdrs.keys()):
    for cc in sorted(hdrs[k], key=lambda cc: cc.count):
        print(f"build $builddir/{k}.stamp: hdrcheck_{cc.rule_id} $srcdir/{k}")
        break
