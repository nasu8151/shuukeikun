# make

subdirs := plugin benchmarks

.PHONY: all $(subdirs)

all: $(subdirs)

$(subdirs):
	$(MAKE) -C $@

clean: $(subdirs:=-clean)

$(subdirs:=-clean):
	$(MAKE) -C $(@:-clean=) clean
