# Generated by devtools/yamaker.

LIBRARY()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/clang16
    contrib/libs/clang16/include
    contrib/libs/clang16/lib
    contrib/libs/clang16/lib/AST
    contrib/libs/clang16/lib/ASTMatchers
    contrib/libs/clang16/lib/Analysis
    contrib/libs/clang16/lib/Basic
    contrib/libs/clang16/lib/Lex
    contrib/libs/clang16/tools/extra/clang-tidy/utils
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/Frontend/OpenMP
    contrib/libs/llvm16/lib/Support
)

ADDINCL(
    contrib/libs/clang16/tools/extra/clang-tidy/altera
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    AlteraTidyModule.cpp
    IdDependentBackwardBranchCheck.cpp
    KernelNameRestrictionCheck.cpp
    SingleWorkItemBarrierCheck.cpp
    StructPackAlignCheck.cpp
    UnrollLoopsCheck.cpp
)

END()
