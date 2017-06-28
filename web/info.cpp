#include "info.h"
#include <sstream>
#include <map>

std::string info_serialize(time_t t, const std::string& category, const vbs_dict_t* dict)
{
	std::ostringstream ss;
	vbs_packer_t pk = VBS_PACKER_INIT(ostream_xio.write, (std::ostream*)&ss, -1);

	vbs_pack_head_of_dict0(&pk);

	// NB: category must be the first
	vbs_pack_cstr(&pk, "c");
	vbs_pack_lstr(&pk, category.c_str(), category.length());

	vbs_pack_cstr(&pk, "t");
	vbs_pack_integer(&pk, t);

	vbs_pack_cstr(&pk, "o");
	if (dict)
	{
		vbs_pack_dict(&pk, dict);
	}
	else
	{
		vbs_pack_head_of_dict0(&pk);
		vbs_pack_tail(&pk);
	}

	vbs_pack_tail(&pk);
	return ss.str();
}

