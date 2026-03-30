install(DIRECTORY easy
        DESTINATION include
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "LLVMHolderImpl.h" EXCLUDE
        PATTERN "RuntimePasses.h" EXCLUDE)
