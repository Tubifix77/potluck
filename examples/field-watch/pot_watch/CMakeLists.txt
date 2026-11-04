# The field watch (ext:watch/two_of_three): an external actor component, as pot_hello is. A cluster file brings it
# into the image with an "apps" entry whose components folder is examples/field-watch. Types 0xA0-0xAF: the block
# DEVELOPING-APPS.md section 5 records for it.
idf_component_register(
    SRCS "src/watch.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pot_actors pot_link pot_ns pot_deploy
)
