real-time based I/O bandwidth reallocation for VM
=================================================

본 프로젝트는...
----------------

가상머신에 설정된 I/O 대역폭을 가상머신이 구동중인 상태(Live)에서도 변경할 수 있는 코드이다.
관련된 기술은 ['링크'](https://www.dbpia.co.kr/journal/articleDetail?nodeId=NODE07450265)에서 확인할 수 있다.

컴파일 방법...
--------------

본 코드는 CentOS 7, qemu-kvm-1.5.3-167 rpm 패키지를 기반으로 작성된다.
<pre>
# git https://github.com/Gh0stSp1der/qemu-kvm.git
# cd qemu-kvm

# ./configure --prefix=/usr --enable-spice --target-list=x86_64-softmmu --enable-kvm
# make

# ${HOME_QEMU-KVM}/x86_64-softmmu/qemu-system-x86_64 --version
QEMU emulator version 1.5.3, Copyright (c) 2003-2008 Fabrice Bellard
</pre>

RPM 패키지 작성 방법...
-----------------------

추후 정리..
