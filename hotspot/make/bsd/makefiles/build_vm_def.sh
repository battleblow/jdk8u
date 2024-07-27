#!/bin/sh

# If we're cross compiling use that path for nm
if [ "$CROSS_COMPILE_ARCH" != "" ]; then
    NM=$ALT_COMPILER_PATH/nm
else
    NM=nm
fi

case "$(uname -s)" in
Darwin )
    $NM -Uj $@ | awk '{
        if ($3 ~ /^_ZTV/ || $3 ~ /^gHotSpotVM/) print "\t" $3
    }' ;;
OpenBSD )
    $NM $@ | awk '{
        if ($2 == "U") next
        if ($3 ~ /^_ZTV/ || $3 ~ /^gHotSpotVM/) print "\t" $3 ";"
        if ($3 ~ /^UseSharedSpaces$/) print "\t" $3 ";"
        if ($3 ~ /^_ZN9Arguments17SharedArchivePathE$/) print "\t" $3 ";"
    }' | sort -u ;;
* )
    $NM --extern-only --defined-only $@ | awk '{
        if ($3 ~ /^_ZTV/ || $3 ~ /^gHotSpotVM/) print "\t" $3 ";"
        if ($3 ~ /^UseSharedSpaces$/) print "\t" $3 ";"
        if ($3 ~ /^_ZN9Arguments17SharedArchivePathE$/) print "\t" $3 ";"
    }' | sort -u ;;
esac
