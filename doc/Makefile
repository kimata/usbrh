CSS         	:= css/doc.css

all: readme.htm

clean:
	rm -f readme.htm

%.htm: %.rst $(CSS)
	rst2html --stylesheet=$(CSS) $< > $@
