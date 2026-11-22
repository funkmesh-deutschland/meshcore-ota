CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror -DMCOTA_BOOT_COMPACT
PYTHON ?= python3
MESHCORE ?= ../../MeshCore-upstream
ED = $(MESHCORE)/lib/ed25519
CORE = ../core/mcota_boot.c ../core/mcota_sha256.c ../core/mcota_patch.c ../core/mcota_manifest.c ../core/mcota_compact.c ../core/mcota_node.c
BOOT = ../core/mcota_boot.c ../core/mcota_sha256.c ../core/mcota_patch.c ../core/mcota_manifest.c ../core/mcota_compact.c
HEADERS = $(wildcard ../core/*.h)

test: test_sha256 test_inflate test_boot test_compact test_node test_esp node_stdio apply_check fixture/update.mcota
	./test_sha256
	./test_inflate fixture/inflate.txt fixture/old.bin fixture/deflate.bin
	$(PYTHON) check_deflate.py fixture/old.bin fixture/deflate.bin
	./apply_check fixture/old.bin fixture/update.mcota fixture/new.bin
	./test_boot
	./test_compact fixture
	./test_node fixture
	./test_esp fixture
	$(PYTHON) test_provider.py fixture ./node_stdio
	$(PYTHON) test_companion.py fixture

test_esp: test_esp.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) -DMCOTA_MAX_PAGES=448u -I$(ED) -o $@ test_esp.c $(CORE) -x c -w $(ED)/verify.c $(ED)/ge.c $(ED)/fe.c $(ED)/sc.c $(ED)/sha512.c

node_stdio: node_stdio.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) -I$(ED) -o $@ node_stdio.c $(CORE) -x c -w $(ED)/verify.c $(ED)/ge.c $(ED)/fe.c $(ED)/sc.c $(ED)/sha512.c

test_sha256: test_sha256.c ../core/mcota_sha256.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test_sha256.c ../core/mcota_sha256.c

test_inflate: test_inflate.c ../core/mcota_patch.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test_inflate.c ../core/mcota_patch.c

apply_check: apply_check.c ../core/mcota_patch.c ../core/mcota_manifest.c $(HEADERS)
	$(CC) $(CFLAGS) -DMCOTA_MAX_PAGES=448u -o $@ apply_check.c ../core/mcota_patch.c ../core/mcota_manifest.c

test_boot: test_boot.c $(BOOT) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test_boot.c $(BOOT)

test_compact: test_compact.c $(BOOT) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test_compact.c $(BOOT)

test_node: test_node.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) -I$(ED) -o $@ test_node.c $(CORE) -x c -w $(ED)/verify.c $(ED)/ge.c $(ED)/fe.c $(ED)/sc.c $(ED)/sha512.c

fixture/update.mcota: make_fixture.py ../tools/mcota.py
	$(PYTHON) make_fixture.py fixture

clean:
	rm -rf apply_check test_boot test_esp test_compact test_sha256 test_inflate test_node node_stdio fixture

.PHONY: test clean
