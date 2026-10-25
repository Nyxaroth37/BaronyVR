
#if 1
	#define WINDOWS
#endif

#if 0
	#define APPLE
#endif

#if 0
	#define LINUX
#endif

#if 0
	#define BSD
#endif

#if 0
	#define HAIKU
#endif

#if 1
	#define STEAMWORKS
#endif

#ifndef EDITOR
	#if 0
		#define USE_EOS
		#define BUILD_ENV_PR ""
		#define BUILD_ENV_SA ""
		#define BUILD_ENV_DE ""
		#define BUILD_ENV_CC ""
		#define BUILD_ENV_CS ""
		#define BUILD_ENV_GSE ""
	#endif
	#if 0
	    #define USE_PLAYFAB
	    #define BUILD_ENV_PFTID ""
	    #define BUILD_ENV_PFHID ""
	#endif
#endif

#if 1
	#define USE_IMGUI
#endif

#if 0
    #define USE_LIBCURL
#endif

#if 0
    #define USE_THEORA_VIDEO
#endif

#if 0
    #define USE_OPUS
#endif

#if 1
	#define USE_FMOD
#endif

#if 0
	#define USE_OPENAL
#endif

#if 0
	#define USE_TREMOR
#endif

#if 0
	#define PANDORA
#endif

#if 0
	/*
	 * https://msdn.microsoft.com/en-us/library/cc230318.aspx
	 * A DWORD is a 32-bit unsigned integer (range: 0 through 4294967295 decimal). Because a DWORD is unsigned, its first bit (Most Significant Bit (MSB)) is not reserved for signing. 
	 * This type is declared as follows:
	 * typedef unsigned long DWORD, *PDWORD, *LPDWORD;
	 */
	typedef unsigned long DWORD;
#endif
