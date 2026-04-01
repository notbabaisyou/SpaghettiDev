#ifndef VACCUM_PRIV_H
#error This file can only be included by vaccum_priv.h
#endif

#ifndef __VACCUM_UTILS_H__
#define __VACCUM_UTILS_H__

#include "vaccum_prepare.h"
#include "mipict.h"

#define vaccum_check_fbo_size(_vaccum_,_w_, _h_)    ((_w_) > 0 && (_h_) > 0 \
                                                    && (_w_) <= _vaccum_->max_fbo_size  \
                                                    && (_h_) <= _vaccum_->max_fbo_size)

#define VACCUM_PIXMAP_PRIV_HAS_FBO(pixmap_priv)    (pixmap_priv->vk_image == VACCUM_IMAGE_NORMAL)

#endif
