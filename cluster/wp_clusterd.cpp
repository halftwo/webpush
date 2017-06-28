#include "ClusterServant.h"
#include "helper/wp_version.h"

static const char *build_info = "$build: webpush-" WP_VERSION " " __DATE__ " " __TIME__ " $";

int run(int argc, char **argv, const xic::EnginePtr& engine)
{
	xic::AdapterPtr adapter = engine->createAdapter();
	ClusterServantPtr cm(new ClusterServant());
	adapter->addServant("wp_cluster", cm);
	adapter->activate();
	engine->throb(build_info);
	engine->waitForShutdown();	
	return 0;
}

int main(int argc, char **argv)
{
	return xic::start_xic_st(run, argc, argv);
}

