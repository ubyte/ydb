/* syntax version 1 */
/* postgres can not */
USE plato;

INSERT INTO @tmp
SELECT
    Just('foo') AS driver_license_pd_id,
    'bar' AS order_id,
    '1' AS user_phone_pd_id,
    '2' AS utc_order_dttm
;

COMMIT;

SELECT
    driver_license_pd_id,
    user_phone_pd_id,
    utc_order_dttm,
    order_id,
    LEAD(
        <|'order_id': order_id, 'order_dttm': utc_order_dttm|>,
        1
    ) OVER (
        PARTITION BY
            user_phone_pd_id
        ORDER BY
            utc_order_dttm
    ) AS next_user_order,
    LEAD(
        <|'order_id': order_id|>,
        1
    ) OVER (
        PARTITION BY
            driver_license_pd_id
        ORDER BY
            utc_order_dttm
    ) AS next_driver_order,
FROM
    @tmp
;
