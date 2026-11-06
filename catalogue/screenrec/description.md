The screen recorder's H.264 encoder for phones with an Allwinner A64. With it installed, PhoneLab can record the
phone's screen as video (`record_start` / `record_stop`, or the record button next to the screenshot one): the
system's recorder captures what you see, skips pictures that didn't change and has the chip's video engine encode the
rest, so recording costs the phone very little.

H.264 is covered by patents in some countries, which is why the encoder isn't part of any Phorepoc image and the
recorder refuses to record without it. Whether to install it is up to you.
