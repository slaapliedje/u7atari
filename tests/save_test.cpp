// --program savetest (builds with U7_DEBUG_LOG): checks the save files keep the PC's byte order.
// Loads each file below from SAVEIN\ in the game folder through the game's own save node, saves
// it again into SAVEOUT\, and leaves the comparison to the host: with a GAMEDAT written by the
// DOS game in SAVEIN\, every file in SAVEOUT\ should match its original byte for byte.
//   cmp SAVEIN/WEATHER.DAT SAVEOUT/WEATHER.DAT, and so on
// Only the nodes that load and save without the rest of the game running are here.
#include "u7port.h"
#include "plat.h"
#include "mapview.h"
#include "datanode.h"
#include "gtimer.h"
#include "partymov.h"
#include "weather.h"
#include "u7ibuf.h"
#include "bogus.h"
#include "missile.h"

extern "C" int16_t StartFarHeap(int16_t unused);

static const char *const Tested[] = {
	"WEATHER.DAT", "MISSTRAC.DAT", "PARTY", "PATH.DAT", "MAPCOORD.DAT", "RANDSEED.DAT",
	"U7_OSTIA.DAT", "GAMETIM",
};

static int tested(const char *name)
{
	for (size_t i = 0; i < sizeof Tested / sizeof Tested[0]; i++)
		if (strncasecmp(name, Tested[i], strlen(Tested[i])) == 0)
			return 1;
	return 0;
}

extern "C" void save_test(void)
{
	char in[] = ".\\SAVEIN\\", out[] = ".\\SAVEOUT\\";

	ResetEnvironment();
	StartFarHeap(0);            /* routes keep their waypoints there */
	plat_dir_create(".\\SAVEOUT");
	for (DataNode *p = SaveNodes; p != 0; p = p->next) {
		if (!tested(p->getName()))
			continue;
		char line[64];

		snprintf(line, sizeof line, "savetest: %s\r\n", p->getName());
		plat_log(line);
		p->load(in);
		p->save(out);
	}
	/* Some of what was read, for checking against the PC's files decoded on the host. */
	char line[200];

	snprintf(line, sizeof line, "savetest values: view %d %d, clock %u %u %u, party %d [%d %d], "
		"weather %d %d %d, vehicle %d, dead %d, quake %d, missiles %d\r\n",
		MainWorldView.centerX.value, MainWorldView.centerY.value, GameTime.ticks, GameTime.days,
		GameTime.rate, PartySize, PartyMembers[0].off, PartyMembers[1].off,
		CurrentWeather.targetCount, CurrentWeather.maxCount, CurrentWeather.frameCount,
		CurrentVehicle, DeadPartyCount, EarthquakeCount, ActiveMissiles);
	plat_log(line);
	plat_log("savetest done\r\n");
}
