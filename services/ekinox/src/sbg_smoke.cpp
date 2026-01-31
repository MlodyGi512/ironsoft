#include "ironsoft/ekinox/sbg_smoke.h"

#include <iostream>

#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
#include "sbgECom.h"
#include <interfaces/sbgInterfaceUdp.h>
#include "sbgErrorCodes.h"
#endif


namespace ironsoft::ekinox {

	void sbg_smoke_version() {
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
		const char* err = sbgEComErrorToString(SBG_NO_ERROR);
		std::cout << "[ekinox] sbgECom linked OK: " << (err ? err : "unknown") << '\n';
#else
		std::cout << "[ekinox] sbgECom disabled (no SDK)" << '\n';
#endif
	}

} // namespace ironsoft::ekinox