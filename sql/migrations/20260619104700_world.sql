DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260619104700');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260619104700');
-- Add your query below.

-- Script 909702 uses TARGET_T_HOSTILE_RANDOM_NOT_TOP (target_type 5), which returns null
-- when only one player is attacking (no target exists outside top aggro).
-- Add SF_GENERAL_SKIP_MISSING_TARGETS so the engine silently skips instead of logging errors.
UPDATE `creature_ai_scripts` SET `data_flags` = (`data_flags` | 16) WHERE `id` = 909702;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
