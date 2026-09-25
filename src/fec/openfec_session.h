/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-15
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _OPENFEC_SESSION_H_
#define _OPENFEC_SESSION_H_

#include <mutex>

namespace minirtc {
// OpenFEC 1.4.2 exposes process-global of_verbosity, written by each instance
// creation and read by other codec calls. Serialize library entry, including
// release, even when individual encoder/decoder instances belong to different
// media threads. Do not hold this lock across application callbacks.
inline std::mutex& OpenFecMutex() {
  static std::mutex mutex;
  return mutex;
}
}  // namespace minirtc
#endif
