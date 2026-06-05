import os

import lit.formats

config.name = 'yansollvm'
config.test_format = lit.formats.ShTest(True)
config.suffixes = ['.c', '.cpp', '.ll']
config.test_source_root = os.path.dirname(__file__)
# Keep all lit-generated %t/%T files under one ignored tree.  If test_exec_root
# is the source tree, tests in subdirectories create sibling Output/ directories
# such as tests/fla-regressions/Output, which are easy to miss in .gitignore.
config.test_exec_root = os.path.join(config.test_source_root, 'Output')

llvm_build = os.environ.get('LLVM_BUILD')
if not llvm_build:
    lit_config.fatal('LLVM_BUILD must point to an LLVM 21 build directory')
plugin = os.environ.get(
    'YANSOLLVM_PLUGIN',
    os.path.abspath(os.path.join(config.test_source_root, '..', 'build', 'yansollvm.so')),
)

config.substitutions.append(('%clang', os.path.join(llvm_build, 'bin', 'clang')))
config.substitutions.append(('%clang\+\+', os.path.join(llvm_build, 'bin', 'clang++')))
config.substitutions.append(('%opt', os.path.join(llvm_build, 'bin', 'opt')))
config.substitutions.append(('%lli', os.path.join(llvm_build, 'bin', 'lli')))
config.substitutions.append(('%FileCheck', os.path.join(llvm_build, 'bin', 'FileCheck')))
config.substitutions.append(('%not', os.path.join(llvm_build, 'bin', 'not')))
config.substitutions.append(('%plugin', plugin))
