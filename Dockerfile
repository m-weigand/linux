FROM debian:bullseye

RUN apt -y update
RUN apt -y upgrade
RUN apt -y install git vim
RUN apt -y install build-essential linux-source bc kmod cpio flex libncurses5-dev libelf-dev libssl-dev dwarves bison
RUN apt -y install gcc-aarch64-linux-gnu
RUN apt -y install rsync rename

RUN mkdir /root/kernel
COPY compile.sh /root/kernel/

ENTRYPOINT /root/kernel/compile.sh

# WORKDIR /root/mesa
# CMD /root/mesa/compile.sh

