
안녕하세요? 스타포스 파이 구매해 주셔서 감사합니다.



일단 첨부한 메모리 카드엔 필요한 세팅이 다 되어 있는 상태입니다.

제가 만든 스타포스파이는 다름 사람들 것과는 달리 HotKey를 추가해 놓은 것이기 때문에, 주요 기능들은 HotKey와의 조합으로 동작하게 되어 있습니다.

기본적으로는 다음과 같이 설정해 놨습니다.



HotKey + Start = 게임 종료

HotKey + 왼쪽 조이스틱 = rewind

HotKey + A = 슬로우모션



이외에도 Retroarch 들어가셔서 입력 - HotKey 설정을 확인하시고, 필요하시다면 추가적인 설정을 하시면 됩니다.



코어나 테마, 게임 업데이트 등은 그 상태에서 그때그때 추가로 진행하면 되겠지만...

(다른 SD 카드를 쓰실 때도 현재의 OS 이미지를 그대로 떠서 옮긴 다음 파티션 확장만 하면 됩니다.)

혹시 이전 것을 대신해 완전히 새로운 OS를 올리고자 하신다면... 

그 이후 추가적으로 다음의 2가지 세팅을 해 줘야만 합니다.



하나는 HotKey 지원하는 조이스틱 드라이버를 올리는 것이고, 다른 하나는 CPU 쿨링팬 동작 스크립트 설치입니다.





HotKey 포함한 조이스틱 드라이버 올리는 법



기본 레트로파이 키패드 드라이버는 HotKey를 지원하지 않습니다.

때문에 HotKey 지원하는 드라이버를 따로 올려야만 합니다.


https://github.com/recalbox/mk_arcade_joystick_rpi/tree/hotkeybtn

여길 보시면 GPIO 2를 HK로 할당하라고 되어 있을 겁니다. (custom 으로 다른 걸 지정하는 것도 가능합니다.)

설치 방법은... 다음과 같습니다.

1. 먼저 레트로파이 설정 메뉴에서 retropie-setup으로 들어가, 드라이버 항목에서 mk_arcade_joystick_rpi 를 제거합니다.
2. 라즈베리파이에 ssh로 접속합니다.
3. 다음의 커맨드를 차례로 입력합니다.

> git clone https://github.com/amos42/mk_arcade_joystick_rpi.git -b hotkeybtn
> cd mk_arcade_joystick_rpi
> utils/makepackage.sh 0.1.6
> sudo dpkg -i build/mk-arcade-joystick-rpi-0.1.6.deb

이렇게 하면 일단 드라이버는 설치 됩니다.

4. 드라이버 설정값을 입력합니다.

> sudo nano /etc/modprobe.d/mk_arcade_joystick.conf

다음 항목을 입력하고 저장합니다.

-------
options mk_arcade_joystick_rpi map=1
-------

5. 이제 이 드라이버를 쓸 수 있게 설정을 추가하면 됩니다.

> sudo nano /etc/modules-load.d/modules.conf

끝에 다음 항목을 추가하고 저장합니다.

----------
mk_arcade_joystick_rpi
----------

6. 리부팅 하면 됩니다.





AutoFan 설정법



라즈겜동의 북마크님 게시물을 보면 됩니다.

요다PCB 역시 같은 핀을 사용하기 때문에 호환 됩니다.



https://cafe.naver.com/raspigamer/14681



