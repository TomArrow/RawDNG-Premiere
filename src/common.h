#pragma once


#define DEBUGOUTPUT

#ifdef DEBUGOUTPUT
extern std::ofstream abasc;
#else
extern std::ofstream nothing;
#define abasc if(0) nothing
#endif
