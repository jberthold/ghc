
-- | A description of the platform we're compiling for.
--
module Platform (
        Platform(..),
        Arch(..),
        OS(..),
        ArmISA(..),
        ArmISAExt(..),

        target32Bit,
        osElfTarget
)

where

import Panic

-- | Contains enough information for the native code generator to emit
--      code for this platform.
data Platform
        = Platform {
              platformArch                     :: Arch,
              platformOS                       :: OS,
              platformHasGnuNonexecStack       :: Bool,
              platformHasSubsectionsViaSymbols :: Bool
          }
        deriving (Read, Show, Eq)


-- | Architectures that the native code generator knows about.
--      TODO: It might be nice to extend these constructors with information
--      about what instruction set extensions an architecture might support.
--
data Arch
        = ArchUnknown
        | ArchX86
        | ArchX86_64
        | ArchPPC
        | ArchPPC_64
        | ArchSPARC
        | ArchARM
          { armISA    :: ArmISA
          , armISAExt :: [ArmISAExt] }
        | ArchAlpha
        | ArchMipseb
        | ArchMipsel
        deriving (Read, Show, Eq)


-- | Operating systems that the native code generator knows about.
--      Having OSUnknown should produce a sensible default, but no promises.
data OS
        = OSUnknown
        | OSLinux
        | OSDarwin
        | OSSolaris2
        | OSMinGW32
        | OSFreeBSD
        | OSOpenBSD
        deriving (Read, Show, Eq)

-- | ARM Instruction Set Architecture and Extensions
--
data ArmISA
    = ARMv5
    | ARMv6
    | ARMv7
    deriving (Read, Show, Eq)

data ArmISAExt
    = VFPv2
    | VFPv3
    | VFPv3D16
    | NEON
    | IWMMX2
    deriving (Read, Show, Eq)


target32Bit :: Platform -> Bool
target32Bit p = case platformArch p of
                ArchUnknown -> panic "Don't know if ArchUnknown is 32bit"
                ArchX86     -> True
                ArchX86_64  -> False
                ArchPPC     -> True
                ArchPPC_64  -> False
                ArchSPARC   -> True
                ArchARM _ _ -> True
                ArchMipseb  -> True
                ArchMipsel  -> True
                ArchAlpha   -> False


-- | This predicates tells us whether the OS supports ELF-like shared libraries.
osElfTarget :: OS -> Bool
osElfTarget OSLinux    = True
osElfTarget OSFreeBSD  = True
osElfTarget OSOpenBSD  = True
osElfTarget OSSolaris2 = True
osElfTarget OSDarwin   = False
osElfTarget OSMinGW32  = False
osElfTarget OSUnknown  = panic "Don't know if OSUnknown is elf"

