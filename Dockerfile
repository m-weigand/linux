FROM debian:bullseye

RUN apt -y update
RUN apt -y upgrade
RUN apt -y install git vim debhelper
RUN apt -y install build-essential linux-source bc kmod cpio flex libncurses5-dev libelf-dev libssl-dev dwarves bison
RUN apt -y install gcc-aarch64-linux-gnu
RUN apt -y install rsync rename

RUN mkdir /root/kernel

COPY compile.sh /root/kernel/

RUN mkdir /root/kernel_v6.2
COPY compile_v6-2.sh /root/kernel_v6.2/

RUN mkdir /root/kernel_v6.3
COPY compile_v6-3.sh /root/kernel_v6.3/

COPY compile_all.sh /root/

ENTRYPOINT /root/compile_all.sh

# WORKDIR /root/mesa
# CMD /root/mesa/compile.sh

