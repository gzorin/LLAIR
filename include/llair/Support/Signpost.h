//-*-C++-*-
#ifndef LLAIR_SUPPORT_SIGNPOST_H
#define LLAIR_SUPPORT_SIGNPOST_H

#if __has_include(<os/signpost.h>)
#include <os/signpost.h>
#define LLAIR_HAVE_SIGNPOST 1
#endif

namespace llair {

#if LLAIR_HAVE_SIGNPOST

// Points-of-Interest category so intervals show up in Instruments' Points of
// Interest track without a custom template. One log for the whole library, so
// intervals emitted from different components share a track.
inline os_log_t
signpostLog() {
    static os_log_t log = os_log_create("com.bourbon.llair", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    return log;
}

#endif

} // End namespace llair

#endif
