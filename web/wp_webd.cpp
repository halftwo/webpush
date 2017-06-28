#include "WebServant.h"
#include "verify.h"
#include "HttpConnection.h"
#include "helper/wp_version.h"
#include "xic/Engine.h"

static const char *build_info = "$build: webpush-" WP_VERSION " " __DATE__ " " __TIME__ " $";

int run(int argc, char **argv, const xic::EnginePtr& engine)
{
	srand(time(NULL));
	verify_init();
	xic::AdapterPtr adapter = engine->createAdapter();
	WebServantPtr ws(new WebServant(adapter, engine->setting()));
	HttpListenerPtr listener(new HttpListener(engine->setting(), ws));
	adapter->activate();
	listener->start();
	engine->throb(build_info);
	engine->waitForShutdown();
	return 0;
}

int main(int argc, char **argv)
{
	SettingPtr setting = newSetting();
	setting->insert("xic.rlimit.nofile", "256ki");
	return xic::start_xic_st(run, argc, argv, setting);
}

