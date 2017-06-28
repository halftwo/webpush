#ifndef info_h_
#define info_h_

#include "xslib/vbs.h"
#include <stdlib.h>
#include <string>

std::string info_serialize(time_t t, const std::string& category, const vbs_dict_t* dict=NULL);

#endif
