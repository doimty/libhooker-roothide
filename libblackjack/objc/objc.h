//
//  objc.h
//  libblackjack
//
//  Created by CoolStar on 2/24/20.
//  Copyright © 2020 CoolStar. All rights reserved.
//

#ifndef objc_h
#define objc_h

#include "libhooker.h"
#include <objc/objc.h>

// ClassPair 在同一安装锁内先发布存根，再让 replacement 对调用者可见。
enum LIBHOOKER_ERR LHHookMessageWithStubs(Class cls, SEL selector, void *replacement, Class stubs);

#endif /* objc_h */
