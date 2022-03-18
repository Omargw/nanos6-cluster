/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2018 Barcelona Supercomputing Center (BSC)
*/

#include "MessageType.hpp"


const char MessageTypeStr[TOTAL_MESSAGE_TYPES][MSG_NAMELEN] =
{
#define HELPER_MACRO(val) #val,
	MESSAGETYPES
#undef HELPER_MACRO
};
