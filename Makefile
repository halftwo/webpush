

dirs = helper web event hub cluster

all:
	for x in $(dirs); do make -C $$x || exit 1; done

clean:
	for x in $(dirs); do (cd $$x; make clean); done
