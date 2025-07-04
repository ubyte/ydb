# Развёртывание {{ ydb-short-name }} кластера вручную

<!-- markdownlint-disable blanks-around-fences -->

Этот документ описывает способ развернуть мультитенантный кластер {{ ydb-short-name }} на нескольких физических или виртуальных серверах.

## Перед началом работы {#before-start}

### Требования {#requirements}

Ознакомьтесь с [системными требованиями](../../../devops/concepts/system-requirements.md) и [топологией кластера](../../../concepts/topology.md).

У вас должен быть SSH доступ на все сервера. Это необходимо для установки артефактов и запуска исполняемого файла {{ ydb-short-name }}.

Сетевая конфигурация должна разрешать TCP соединения по следующим портам (по умолчанию, могут быть изменены настройками):

* 22: сервис SSH;
* 2135, 2136: GRPC для клиент-кластерного взаимодействия;
* 19001, 19002: Interconnect для внутрикластерного взаимодействия узлов;
* 8765, 8766: HTTP интерфейс {{ ydb-short-name }} Embedded UI.

При размещении нескольких динамических узлов на одном сервере потребуются отдельные порты для gRPC, Interconnect и HTTP интерфейса каждого динамического узла в рамках сервера.

Убедитесь в том, что системные часы на всех серверах в составе кластера синхронизированы с помощью инструментов `ntpd` или `chrony`. Желательно использовать единый источник времени для всех серверов кластера, чтобы обеспечить одинаковую обработку секунд координации (leap seconds).

Если применяемый на серверах кластера тип Linux использует `syslogd` для логирования, необходимо настроить ротацию файлов лога с использованием инструмента `logrotate` или его аналогов. Сервисы {{ ydb-short-name }}  могут генерировать значительный объем системных логов, в особенности при повышении уровня логирования для диагностических целей, поэтому важно включить ротацию файлов системного лога для исключения ситуаций переполнения файловой системы `/var`.

Выберите серверы и диски, которые будут использоваться для хранения данных:

* Используйте схему отказоустойчивости `block-4-2` для развертывания кластера в одной зоне доступности (AZ), задействуя не менее 8 серверов. Данная схема позволяет переживать отказ 2 серверов.
* Используйте схему отказоустойчивости `mirror-3-dc` для развертывания кластера в трех зонах доступности (AZ), задействуя не менее 9 серверов. Данная схема позволяет переживать отказ 1 AZ и 1 сервера в другой AZ. Количество задействованных серверов в каждой AZ должно быть одинаковым.

{% note info %}

Запускайте каждый статический узел (узел хранения данных) на отдельном сервере. Возможно совмещение статических и динамических узлов на одном сервере, а также размещение на одном сервере нескольких динамических узлов при наличии достаточных вычислительных ресурсов.

{% endnote %}

Подробнее требования к оборудованию описаны в разделе [{#T}](../../../devops/concepts/system-requirements.md).

### Подготовка ключей и сертификатов TLS {#tls-certificates}

Защита трафика и проверка подлинности серверных узлов {{ ydb-short-name }} осуществляется с использованием протокола TLS. Перед установкой кластера необходимо спланировать состав серверов, определиться со схемой именования узлов и конкретными именами, и подготовить ключи и сертификаты TLS.

Вы можете использовать существующие или сгенерировать новые сертификаты. Следующие файлы ключей и сертификатов TLS должны быть подготовлены в формате PEM:

* `ca.crt` - сертификат центра регистрации (Certification Authority, CA), которым подписаны остальные сертификаты TLS (одинаковые файлы на всех узлах кластера);
* `node.key` - секретные ключи TLS для каждого из узлов кластера (свой ключ на каждый сервер кластера);
* `node.crt` - сертификаты TLS для каждого из узлов кластера (соответствующий ключу сертификат);
* `web.pem` - конкатенация секретного ключа узла, сертификата узла и сертификата центра регистрации для работы HTTP интерфейса мониторинга (свой файл на каждый сервер кластера).

Необходимые параметры формирования сертификатов определяются политикой организации. Обычно сертификаты и ключи для {{ ydb-short-name }} формируются со следующими параметрами:

* ключи RSA длиною 2048 или 4096 бит;
* алгоритм подписи сертификатов SHA-256 с шифрованием RSA;
* срок действия сертификатов узлов не менее 1 года;
* срок действия сертификата центра регистрации не менее 3 лет.

Необходимо, чтобы сертификат центра регистрации был помечен соответствующим образом: должен быть установлен признак CA, а также включены виды использования "Digital Signature, Non Repudiation, Key Encipherment, Certificate Sign".

Для сертификатов узлов важно соответствие фактического имени хоста (или имён хостов) значениям, указанным в поле "Subject Alternative Name". Для сертификатов должны быть включены виды использования "Digital Signature, Key Encipherment" и расширенные виды использования "TLS Web Server Authentication, TLS Web Client Authentication". Необходимо, чтобы сертификаты узлов поддерживали как серверную, так и клиентскую аутентификацию (опция `extendedKeyUsage = serverAuth,clientAuth` в настройках OpenSSL).

Для пакетной генерации или обновления сертификатов кластера {{ ydb-short-name }} с помощью программного обеспечения OpenSSL можно воспользоваться [примером скрипта](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/tls_cert_gen/), размещённым в репозитории {{ ydb-short-name }} на GitHub. Скрипт позволяет автоматически сформировать необходимые файлы ключей и сертификатов для всего набора узлов кластера за одну операцию, облегчая подготовку к установке.

## Создайте системного пользователя и группу, от имени которых будет работать {{ ydb-short-name }} {#create-user}

На каждом сервере, где будет запущен {{ ydb-short-name }}, выполните:

```bash
sudo groupadd ydb
sudo useradd ydb -g ydb
```

Для того, чтобы сервис {{ ydb-short-name }} имел доступ к блочным дискам для работы, необходимо добавить пользователя, под которым будут запущены процессы {{ ydb-short-name }}, в группу `disk`:

```bash
sudo usermod -aG disk ydb
```

## Установите программное обеспечение {{ ydb-short-name }} на каждом сервере {#install-binaries}

1. Скачайте и распакуйте архив с исполняемым файлом `ydbd` и необходимыми для работы {{ ydb-short-name }} библиотеками:

    ```bash
    mkdir ydbd-stable-linux-amd64
    curl -L {{ ydb-binaries-url }}/{{ ydb-stable-binary-archive }} | tar -xz --strip-component=1 -C ydbd-stable-linux-amd64
    ```

1. Скопируйте исполняемый файл и библиотеки в соответствующие директории:

    ```bash
    sudo cp -iR ydbd-stable-linux-amd64/bin /opt/ydb/
    sudo cp -iR ydbd-stable-linux-amd64/lib /opt/ydb/
    ```

1. Установите владельца файлов и каталогов:

    ```bash
    sudo chown -R root:bin /opt/ydb
    ```

## Подготовьте и очистите диски на каждом сервере {#prepare-disks}

{% include [_includes/storage-device-requirements.md](../../../_includes/storage-device-requirements.md) %}

Получить список блочных устройств на сервере можно командой `lsblk`. Пример вывода:

```txt
NAME   MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS
loop0    7:0    0  63.3M  1 loop /snap/core20/1822
...
vda    252:0    0    40G  0 disk
├─vda1 252:1    0     1M  0 part
└─vda2 252:2    0    40G  0 part /
vdb    252:16   0   186G  0 disk
└─vdb1 252:17   0   186G  0 part
```

Названия блочных устройств зависят от настроек операционной системы, заданных базовым образом или настроенных вручную. Обычно имена устройств состоят из трех частей:

- Фиксированный префикс или префикс, указывающий на тип устройства
- Последовательный идентификатор устройства (может быть буквой или числом)
- Последовательный идентификатор раздела на данном устройстве (обычно число)

1. Создайте разделы на выбранных дисках:

    {% note alert %}

    Следующая операция удалит все разделы на указанном диске! Убедитесь, что вы указали диск, на котором нет других данных!

    {% endnote %}

    ```bash
    DISK=/dev/nvme0n1
    sudo parted ${DISK} mklabel gpt -s
    sudo parted -a optimal ${DISK} mkpart primary 0% 100%
    sudo parted ${DISK} name 1 ydb_disk_ssd_01
    sudo partx --u ${DISK}
    ```

    Выполните команду `ls -l /dev/disk/by-partlabel/`, чтобы убедиться что в системе появился диск с меткой `/dev/disk/by-partlabel/ydb_disk_ssd_01`.

    Если вы планируете использовать более одного диска на каждом сервере, укажите для каждого свою уникальную метку вместо `ydb_disk_ssd_01`. Метки дисков должны быть уникальны в рамках каждого сервера, и используются в конфигурационных файлах, как показано в последующих инструкциях.

    Для упрощения последующей настройки удобно использовать одинаковые метки дисков на серверах кластера, имеющих идентичную конфигурацию дисков.

2. Очистите диск встроенной в исполняемый файл `ydbd` командой:

    {% note warning %}

    После выполнения команды данные на диске сотрутся.

    {% endnote %}

    ```bash
    sudo LD_LIBRARY_PATH=/opt/ydb/lib /opt/ydb/bin/ydbd admin bs disk obliterate /dev/disk/by-partlabel/ydb_disk_ssd_01
    ```

    Проделайте данную операцию для каждого диска, который будет использоваться для хранения данных {{ ydb-short-name }}.

## Подготовьте конфигурационные файлы {#config}

Подготовьте конфигурационный файл {{ ydb-short-name }}:

1. Скачайте пример конфига для соответствующей модели отказа вашего кластера:

    * [block-4-2](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/yaml_config_examples/block-4-2.yaml) - для однодатацентрового кластера.
    * [mirror-3dc](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/yaml_config_examples/mirror-3dc-9-nodes.yaml) - для cross-DC кластера из 9 нод.
    * [mirror-3dc-3nodes](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/yaml_config_examples/mirror-3dc-3-nodes.yaml) - для cross-DC кластера из 3 нод.

1. В секции `host_configs` укажите все диски и их тип на каждой из нод кластера. Возможные варианты типов дисков:

    * ROT: rotational, HDD диски.
    * SSD: SSD или NVMe диски.

    ```yaml
    host_configs:
    - drive:
      - path: /dev/disk/by-partlabel/ydb_disk_ssd_01
        type: SSD
      host_config_id: 1
    ```

1. В секции `hosts` укажите FQDN всех нод, их конфигурацию и расположение по датацентрам (`data_center`) и стойкам (`rack`):

    ```yaml
    hosts:
    - host: node1.ydb.tech
      host_config_id: 1
      walle_location:
        body: 1
        data_center: 'zone-a'
        rack: '1'
    - host: node2.ydb.tech
      host_config_id: 1
      walle_location:
        body: 2
        data_center: 'zone-b'
        rack: '1'
    - host: node3.ydb.tech
      host_config_id: 1
      walle_location:
        body: 3
        data_center: 'zone-c'
        rack: '1'
    ```

1. Включите аутентификацию пользователей (опционально).

    Если вы планируете использовать в кластере {{ ydb-short-name }} возможности аутентификации и разграничения доступа пользователей, добавьте секцию `security_config` со следующими параметрами:

    ```yaml
    security_config:
      enforce_user_token_requirement: true
      monitoring_allowed_sids:
      - "root"
      - "ADMINS"
      - "DATABASE-ADMINS"
      administration_allowed_sids:
      - "root"
      - "ADMINS"
      - "DATABASE-ADMINS"
      viewer_allowed_sids:
      - "root"
      - "ADMINS"
      - "DATABASE-ADMINS"
    ```

При использовании режима шифрования трафика убедитесь в наличии в конфигурационном файле {{ ydb-short-name }} установленных путей к файлам ключей и сертификатов в секциях `interconnect_config` и `grpc_config`:

```yaml
interconnect_config:
  start_tcp: true
  encryption_mode: OPTIONAL
  path_to_certificate_file: "/opt/ydb/certs/node.crt"
  path_to_private_key_file: "/opt/ydb/certs/node.key"
  path_to_ca_file: "/opt/ydb/certs/ca.crt"
grpc_config:
  cert: "/opt/ydb/certs/node.crt"
  key: "/opt/ydb/certs/node.key"
  ca: "/opt/ydb/certs/ca.crt"
  services_enabled:
  - legacy
```

Сохраните конфигурационный файл {{ ydb-short-name }} под именем `/tmp/config.yaml` на каждом сервере кластера.

Более подробная информация по созданию файла конфигурации приведена в разделе [{#T}](../../../devops/configuration-management/configuration-v2/config-settings.md).

## Скопируйте ключи и сертификаты TLS на каждый сервер {#tls-copy-cert}

Подготовленные ключи и сертификаты TLS необходимо скопировать в защищенный каталог на каждом из узлов кластера {{ ydb-short-name }}. Ниже приведен пример команд для создания защищенного каталога и копирования файлов с ключами и сертификатами.

```bash
sudo mkdir -p /opt/ydb/certs
sudo cp -v ca.crt /opt/ydb/certs/
sudo cp -v node.crt /opt/ydb/certs/
sudo cp -v node.key /opt/ydb/certs/
sudo cp -v web.pem /opt/ydb/certs/
sudo chown -R ydb:ydb /opt/ydb/certs
sudo chmod 700 /opt/ydb/certs
```

## Подготовьте конфигурацию на статических узлах кластера

Создайте на каждой машине пустую директорию `opt/ydb/cfg` для работы кластера с конфигурацией. В случае запуска нескольких узлов кластера на одной машине создайте отдельные директории под каждый узел. Выполнив специальную команду на каждой машине, инициализируйте эту директорию файлом конфигурации.

```bash
sudo mkdir -p /opt/ydb/cfg
sudo chown -R ydb:ydb /opt/ydb/cfg
ydb admin node config init --config-dir /opt/ydb/cfg --from-config /tmp/config.yaml
```

Исходный файл `/tmp/config.yaml` после выполнения этой команды больше не используется, его можно удалить.

## Запустите статические узлы {#start-storage}

{% list tabs group=manual-systemd %}

- Вручную

  Запустите сервис хранения данных {{ ydb-short-name }} на каждом статическом узле кластера:

  ```bash
  sudo su - ydb
  cd /opt/ydb
  export LD_LIBRARY_PATH=/opt/ydb/lib
  /opt/ydb/bin/ydbd server --log-level 3 --syslog --tcp --config-dir /opt/ydb/cfg \
      --grpcs-port 2135 --ic-port 19001 --mon-port 8765 --mon-cert /opt/ydb/certs/web.pem --node static
  ```

- С использованием systemd

  Создайте на каждом сервере, где будет размещен статический узел кластера, конфигурационный файл systemd `/etc/systemd/system/ydbd-storage.service` по приведенному ниже образцу. Образец файла также можно [скачать из репозитория](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/systemd_services/ydbd-storage.service).

  ```ini
  [Unit]
  Description=YDB storage node
  After=network-online.target rc-local.service
  Wants=network-online.target
  StartLimitInterval=10
  StartLimitBurst=15

  [Service]
  Restart=always
  RestartSec=1
  User=ydb
  PermissionsStartOnly=true
  StandardOutput=syslog
  StandardError=syslog
  SyslogIdentifier=ydbd
  SyslogFacility=daemon
  SyslogLevel=err
  Environment=LD_LIBRARY_PATH=/opt/ydb/lib
  ExecStart=/opt/ydb/bin/ydbd server --log-level 3 --syslog --tcp \
      --config-dir /opt/ydb/cfg \
      --grpcs-port 2135 --ic-port 19001 --mon-port 8765 \
      --mon-cert /opt/ydb/certs/web.pem --node static
  LimitNOFILE=65536
  LimitCORE=0
  LimitMEMLOCK=3221225472

  [Install]
  WantedBy=multi-user.target
  ```

  Запустите сервис на каждом статическом узле {{ ydb-short-name }}:

  ```bash
  sudo systemctl start ydbd-storage
  ```

{% endlist %}

## Инициализируйте кластер {#initialize-cluster}

Операция инициализации кластера осуществляет настройку набора статических узлов, перечисленных в конфигурационном файле кластера, для хранения данных {{ ydb-short-name }}.

Для инициализации кластера потребуется файл сертификата центра регистрации `ca.crt`, путь к которому должен быть указан при выполнении соответствующих команд. Перед выполнением соответствующих команд скопируйте файл `ca.crt` на сервер, на котором эти команды будут выполняться.

Порядок действий по инициализации кластера зависят от того, включен ли в конфигурационном файле {{ ydb-short-name }} режим аутентификации пользователей.

{% list tabs group=authentication %}

- Аутентификация включена

  Для выполнения административных команд (включая инициализацию кластера, создание баз данных, управление дисками и другие) в кластере со включённым режимом аутентификации пользователей необходимо предварительно получить аутентификационный токен с использованием клиента {{ ydb-short-name }} CLI версии 2.0.0 или выше. Клиент {{ ydb-short-name }} CLI следует установить на любом компьютере, имеющем сетевой доступ к узлам кластера (например, на одном из узлов кластера), в соответствии с [инструкцией по установке](../../../reference/ydb-cli/install.md).

  При первоначальной установке кластера в нём существует единственная учётная запись `root` с пустым паролем, поэтому команда получения токена выглядит следующим образом:

  ```bash
  ydb -e grpcs://<node1.ydb.tech>:2135 -d /Root --ca-file ca.crt \
       --user root --no-password auth get-token --force > token-file
  ```

  В качестве сервера для подключения (параметр `-e` или `--endpoint`) может быть указан любой из серверов хранения в составе кластера.

  При успешном выполнении указанной выше команды аутентификационный токен будет записан в файл `token-file`. Файл токена необходимо скопировать на один из серверов хранения в составе кластера, а затем на выбранном сервере выполнить команды:

  ```bash
  export LD_LIBRARY_PATH=/opt/ydb/lib
  ydb --token-file token-file --ca-file ca.crt -e grpcs://<node.ydb.tech>:2135 \
      admin cluster bootstrap --uuid <строка>
  echo $?
  ```

- Аутентификация выключена

  На одном из серверов хранения в составе кластера выполните команды:

  ```bash
  export LD_LIBRARY_PATH=/opt/ydb/lib
  ydb --ca-file ca.crt -e grpcs://<node.ydb.tech>:2135 \
      admin cluster bootstrap --uuid <строка>
  echo $?
  ```

{% endlist %}

При успешном выполнении инициализации кластера выведенный на экран код завершения команды инициализации кластера должен быть нулевым.

## Создайте базу данных {#create-db}

Для работы со строковыми или колоночными таблицами необходимо создать как минимум одну базу данных и запустить процесс или процессы, обслуживающие эту базу данных (динамические узлы).

Для выполнения административной команды создания базы данных потребуется файл сертификата центра регистрации `ca.crt`, аналогично описанному выше порядку выполнения действий по инициализации кластера.

При создании базы данных устанавливается первоначальное количество используемых групп хранения, определяющее доступную пропускную способность ввода-вывода и максимальную емкость хранения. Количество групп хранения может быть при необходимости увеличено после создания базы данных.

Порядок действий по созданию базы данных зависит от того, включен ли в конфигурационном файле {{ ydb-short-name }} режим аутентификации пользователей.

{% list tabs group=authentication %}

- Аутентификация включена

  Необходимо получить аутентификационный токен. Может использоваться файл с токеном аутентификации, полученный при выполнении [инициализации кластера](#initialize-cluster), либо подготовлен новый токен.

  Файл токена необходимо скопировать на один из серверов хранения в составе кластера, а затем на выбранном сервере выполнить команды:

  ```bash
  export LD_LIBRARY_PATH=/opt/ydb/lib
  /opt/ydb/bin/ydbd -f token-file --ca-file ca.crt -s grpcs://`hostname -f`:2135 \
      admin database /Root/testdb create ssd:1
  echo $?
  ```

- Аутентификация выключена

  На одном из серверов хранения в составе кластера выполните команды:

  ```bash
  export LD_LIBRARY_PATH=/opt/ydb/lib
  /opt/ydb/bin/ydbd --ca-file ca.crt -s grpcs://`hostname -s`:2135 \
      admin database /Root/testdb create ssd:1
  echo $?
  ```

{% endlist %}

При успешном создании базы данных, выведенный на экран код завершения команды должен быть нулевым.

В приведенном выше примере команд используются следующие параметры:

* `/Root` - имя корневого домена, должно соответствовать настройке `domains_config`.`domain`.`name` в файле конфигурации кластера;
* `testdb` - имя создаваемой базы данных;
* `ssd:1` - имя пула хранения и количество выделяемых групп хранения. Имя пула обычно означает тип устройств хранения данных и должно соответствовать настройке `storage_pool_types`.`kind` внутри элемента `domains_config`.`domain` файла конфигурации.

Создайте на каждом динамическом узле директорию `/opt/ydb/cfg` для работы кластера с конфигурацией. В случае поднятия нескольких узлов на одной машине, используйте одну и ту же директорию. Выполнив специальную команду на каждой машине, инициализируйте эту директорию с использованием произвольного статического узла кластера в качестве источника конфигурации.

```bash
sudo mkdir -p /opt/ydb/cfg
sudo chown -R ydb:ydb /opt/ydb/cfg
ydb admin node config init --config-dir /opt/ydb/cfg --seed-node <node.ydb.tech:2135>
```

В примере команды выше `<node.ydb.tech>` - FQDN статического узла кластера, с которого будет загружен файл конфигурации.

## Запустите динамические узлы {#start-dynnode}

{% list tabs group=manual-systemd %}

- Вручную

  Запустите динамический узел {{ ydb-short-name }} для базы `/Root/testdb`:

  ```bash
  sudo su - ydb
  cd /opt/ydb
  export LD_LIBRARY_PATH=/opt/ydb/lib
  /opt/ydb/bin/ydbd server --grpcs-port 2136 --grpc-ca /opt/ydb/certs/ca.crt \
      --ic-port 19002 --ca /opt/ydb/certs/ca.crt \
      --mon-port 8766 --mon-cert /opt/ydb/certs/web.pem \
      --config-dir /opt/ydb/cfg \
      --tenant /Root/testdb \
      --node-broker grpcs://<ydb1>:2135 \
      --node-broker grpcs://<ydb2>:2135 \
      --node-broker grpcs://<ydb3>:2135
  ```

  В примере команды выше `<ydbN>` - FQDN трех любых серверов, на которых запущены статические узлы кластера.

- С использованием systemd

  Создайте конфигурационный файл systemd `/etc/systemd/system/ydbd-testdb.service` по приведенному ниже образцу. Образец файла также можно [скачать из репозитория](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/systemd_services/ydbd-testdb.service).

  ```ini
  [Unit]
  Description=YDB testdb dynamic node
  After=network-online.target rc-local.service
  Wants=network-online.target
  StartLimitInterval=10
  StartLimitBurst=15

  [Service]
  Restart=always
  RestartSec=1
  User=ydb
  PermissionsStartOnly=true
  StandardOutput=syslog
  StandardError=syslog
  SyslogIdentifier=ydbd
  SyslogFacility=daemon
  SyslogLevel=err
  Environment=LD_LIBRARY_PATH=/opt/ydb/lib
  ExecStart=/opt/ydb/bin/ydbd server \
      --grpcs-port 2136 --grpc-ca /opt/ydb/certs/ca.crt \
      --ic-port 19002 --ca /opt/ydb/certs/ca.crt \
      --mon-port 8766 --mon-cert /opt/ydb/certs/web.pem \
      --config-dir /opt/ydb/cfg \
      --tenant /Root/testdb \
      --node-broker grpcs://<ydb1>:2135 \
      --node-broker grpcs://<ydb2>:2135 \
      --node-broker grpcs://<ydb3>:2135
  LimitNOFILE=65536
  LimitCORE=0
  LimitMEMLOCK=32212254720

  [Install]
  WantedBy=multi-user.target
  ```

  В примере файла выше `<ydbN>` - FQDN трех любых серверов, на которых запущены статические узлы кластера.

  Запустите динамический узел {{ ydb-short-name }} для базы `/Root/testdb`:

  ```bash
  sudo systemctl start ydbd-testdb
  ```

{% endlist %}

Запустите дополнительные динамические узлы на других серверах для масштабирования и обеспечения отказоустойчивости базы данных.

## Первоначальная настройка учетных записей {#security-setup}

Если в файле настроек кластера включен режим аутентификации, то перед началом работы с кластером {{ ydb-short-name }} необходимо выполнить первоначальную настройку учетных записей.

При первоначальной установке кластера {{ ydb-short-name }} автоматически создается учетная запись `root` с пустым паролем, а также стандартный набор групп пользователей, описанный в разделе [{#T}](../../../security/builtin-security.md).

Для выполнения первоначальной настройки учетных записей в созданном кластере {{ ydb-short-name }} выполните следующие операции:

1. Установите {{ ydb-short-name }} CLI, как описано в [документации](../../../reference/ydb-cli/install.md).

1. Выполните установку пароля учетной записи `root`:

    ```bash
    ydb --ca-file ca.crt -e grpcs://<node.ydb.tech>:2136 -d /Root/testdb --user root --no-password \
        yql -s 'ALTER USER root PASSWORD "passw0rd"'
    ```

    Вместо значения `passw0rd` подставьте необходимый пароль. Сохраните пароль в отдельный файл. Последующие команды от имени пользователя `root` будут выполняться с использованием пароля, передаваемого с помощью ключа `--password-file <path_to_user_password>`. Также пароль можно сохранить в профиле подключения, как описано в [документации {{ ydb-short-name }} CLI](../../../reference/ydb-cli/profile/index.md).

1. Создайте дополнительные учетные записи:

    ```bash
    ydb --ca-file ca.crt -e grpcs://<node.ydb.tech>:2136 -d /Root/testdb --user root --password-file <path_to_root_pass_file> \
        yql -s 'CREATE USER user1 PASSWORD "passw0rd"'
    ```

1. Установите права учетных записей, включив их во встроенные группы:

    ```bash
    ydb --ca-file ca.crt -e grpcs://<node.ydb.tech>:2136 -d /Root/testdb --user root --password-file <path_to_root_pass_file> \
        yql -s 'ALTER GROUP `ADMINS` ADD USER user1'
    ```

В перечисленных выше примерах команд `<node.ydb.tech>` — FQDN сервера, на котором запущен любой динамический узел, обслуживающий базу `/Root/testdb`. При подключении по SSH к динамическому узлу {{ ydb-short-name }} удобно использовать конструкцию `grpcs://$(hostname -f):2136` для получения FQDN.

При выполнении команд создания учётных записей и присвоения групп клиент {{ ydb-short-name }} CLI будет запрашивать ввод пароля пользователя `root`. Избежать многократного ввода пароля можно, создав профиль подключения, как описано в [документации {{ ydb-short-name }} CLI](../../../reference/ydb-cli/profile/index.md).

## Протестируйте работу с созданной базой {#try-first-db}

1. Установите {{ ydb-short-name }} CLI, как описано в [документации](../../../reference/ydb-cli/install.md).

1. Создайте тестовую строковую (`test_row_table`) или колоночную таблицу (`test_column_table`):

{% list tabs %}

- Создание строковой таблицы

    ```bash
    ydb --ca-file ca.crt -e grpcs://<node.ydb.tech>:2136 -d /Root/testdb --user root \
        yql -s 'CREATE TABLE `testdir/test_row_table` (id Uint64, title Utf8, PRIMARY KEY (id));'
    ```

- Создание колоночной таблицы

    ```bash
    ydb --ca-file ca.crt -e grpcs://<node.ydb.tech>:2136 -d /Root/testdb --user root \
        yql -s 'CREATE TABLE `testdir/test_column_table` (id Uint64, title Utf8, PRIMARY KEY (id)) WITH (STORE = COLUMN);'
    ```

{% endlist %}

Где `<node.ydb.tech>` - FQDN сервера, на котором запущен динамический узел, обслуживающий базу `/Root/testdb`.

## Проверка доступа ко встроенному web-интерфейсу

Для проверки доступа ко встроенному web-интерфейсу {{ ydb-short-name }} достаточно открыть в Web-браузере страницу с адресом `https://<node.ydb.tech>:8765`, где `<node.ydb.tech>` - FQDN сервера, на котором запущен любой статический узел {{ ydb-short-name }}.

В Web-браузере должно быть настроено доверие в отношении центра регистрации, выпустившего сертификаты для кластера {{ ydb-short-name }}, в противном случае будет отображено предупреждение об использовании недоверенного сертификата.

Если в кластере включена аутентификация, в Web-браузере должен отобразиться запрос логина и пароля. После ввода верных данных аутентификации должна отобразиться начальная страница встроенного web-интерфейса. Описание доступных функций и пользовательского интерфейса приведено в разделе [{#T}](../../../reference/embedded-ui/index.md).

{% note info %}

Обычно для обеспечения доступа ко встроенному web-интерфейсу {{ ydb-short-name }} настраивают отказоустойчивый HTTP-балансировщик на базе программного обеспечения `haproxy`, `nginx` или аналогов. Детали настройки HTTP-балансировщика выходят за рамки стандартной инструкции по установке {{ ydb-short-name }}.

{% endnote %}


## Особенности установки {{ ydb-short-name }} в незащищенном режиме

{% note warning %}

Мы не рекомендуем использовать незащищенный режим работы {{ ydb-short-name }} ни при эксплуатации, ни при разработке приложений.

{% endnote %}

Описанная выше процедура установки предусматривает развёртывание {{ ydb-short-name }} в стандартном защищенном режиме.

Незащищённый режим работы {{ ydb-short-name }} предназначен для решения тестовых задач, преимущественно связанных с разработкой и тестированием программного обеспечения {{ ydb-short-name }}. В незащищенном режиме:

* трафик между узлами кластера, а также между приложениями и кластером использует незашифрованные соединения;
* не используется аутентификация пользователей (включение аутентификации при отсутствии шифрования трафика не имеет смысла, поскольку логин и пароль в такой конфигурации передавались бы через сеть в открытом виде).

Установка {{ ydb-short-name }} для работы в незащищенном режиме производится в порядке, описанном выше, со следующими исключениями:

1. При подготовке к установке не требуется формировать сертификаты и ключи TLS, и не выполняется копирование сертификатов и ключей на узлы кластера.
1. Из конфигурационных файлов кластерных узлов исключается подсекция `security_config` в секции `domains_config`, а также целиком исключаются секции `interconnect_config` и `grpc_config`.
1. Используются упрощенный вариант команд запуска статических и динамических узлов кластера: исключаются опции с именами файлов сертификатов и ключей, используется протокол `grpc` вместо `grpcs` при указании точек подключения.
1. Пропускается ненужный в незащищенном режиме шаг по получению токена аутентификации перед выполнением инициализации кластера и созданием базы данных.
1. Команда инициализации кластера выполняется в следующей форме:

    ```bash
    export LD_LIBRARY_PATH=/opt/ydb/lib
    ydb admin cluster bootstrap --uuid <строка>
    echo $?
    ```

1. Команда создания базы данных выполняется в следующей форме:

    ```bash
    export LD_LIBRARY_PATH=/opt/ydb/lib
    /opt/ydb/bin/ydbd admin database /Root/testdb create ssd:1
    ```

1. При обращении к базе данных из {{ ydb-short-name }} CLI и приложений используется протокол grpc вместо grpcs, и не используется аутентификация.
