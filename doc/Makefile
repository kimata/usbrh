CSS         	:= css/doc.css

all: readme.htm

clean:
	rm -f readme.htm

%.htm: %.txt $(CSS)
	rst2html --stylesheet=$(CSS) $< > $@
